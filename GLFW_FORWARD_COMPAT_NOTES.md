# GLFW `BadValue` on Linux after wrapping hints in `#ifdef __APPLE__`

> **Symptom**
> ```
> GLFW error 65543: GLX: Failed to create context: BadValue (integer parameter out of range for operation)
> Failed to create GLFW window
> ```
> ...even after adding:
> ```c
> #ifdef __APPLE__
>     glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
>     glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
>     glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
>     glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
> #endif /* __APPLE__ */
> ```

Short answer: almost certainly **not your driver** — it's almost always one of these, in order of likelihood.

## 1. You're running a stale binary, or patched only one of the call sites

There are three places that set these hints in this repo:

- `src/main.cpp` (line ~863)
- `src/gateway.cpp` (line ~753)
- `pexninja/pexninja.cpp` (line ~3251, already guarded with `HOST_DARWIN`)

If the binary you're running comes from a different source file than the one you
edited (e.g. you edited `main.cpp` but launched `pulse-gateway`, or vice versa),
the unpatched copy is still calling
`glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE)` and you'll get the exact
same `BadValue`. Rebuild and double-check which executable produces the error —
`grep`-confirm the hint is gone from the actual `.cpp` that gets linked into it.

## 2. GLFW window hints are sticky across calls

`glfwWindowHint` state persists until `glfwInit()` or `glfwDefaultWindowHints()`
is called. If anything earlier in the process (another module, a previous
`glfwCreateWindow` attempt, an `ImGui_ImplGlfw_*` helper, etc.) set
`GLFW_OPENGL_FORWARD_COMPAT`, your `#ifdef`'d block won't unset it on Linux.
Add an explicit reset right before your hints on the non-Apple path:

```c
glfwDefaultWindowHints();
#ifdef __APPLE__
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    ...
#endif
```

## 3. On Linux you now request no version at all — fine for GLX, but ImGui's GL3 backend won't be happy

With everything inside `#ifdef __APPLE__`, on Linux you fall back to GLFW's
defaults (no version/profile requested). GLX will happily give you a legacy
context, so `glfwCreateWindow` should succeed. If it still fails with
`BadValue`, that strongly points back to (1) or (2). Note that once you do get
the window, ImGui's GL3 backend expects a 3.2+ core context, so you'll want:

```c
glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
```

i.e. **only the `FORWARD_COMPAT` line needs to be Apple-only** — that's what
`pexninja.cpp` already does. The 3.2 core hints themselves are fine on Linux;
it's specifically `GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB` combined with a
compatibility-profile or sub-3.0 context that Mesa/NVIDIA reject with
`BadValue`.

## Things that would actually point at your driver

Rule these out if (1)/(2) don't fix it:

- `glxinfo | grep "OpenGL version"` — needs to report ≥ 3.2.
- Running over SSH without X forwarding, inside a container without `/dev/dri`,
  on Wayland without XWayland, or with `LIBGL_ALWAYS_SOFTWARE=1` falling back
  to `llvmpipe` — all can produce odd GLX errors, though usually not this exact one.
- An NVIDIA driver/kernel-module version mismatch (`nvidia-smi` failing is a
  giveaway).

But the symptom you're describing — same error after wrapping the hint — is
overwhelmingly "the hint is still being set somewhere," not a driver bug.
Check which binary you're running and whether all three call sites are covered.
