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

---

## Update: `glxinfo` itself fails with the same error

```
$ glxinfo | grep "OpenGL version"
X Error of failed request:  BadValue (integer parameter out of range for operation)
  Major opcode of failed request:  152 (GLX)
  Minor opcode of failed request:  24 (X_GLXCreateNewContext)
  Value in failed request:  0x0
  Serial number of failed request:  110
```

This **rules out Doppler entirely**. `glxinfo` doesn't call any of our code and
doesn't set forward-compat/core-profile/version hints — it just asks the X
server for a basic GLX context via `glXCreateNewContext`. The server is saying
"no" with `BadValue` to that bare request, which is a system-level GL/X
problem. Patching `glfwWindowHint` cannot fix this, which is why the earlier
`#ifdef __APPLE__` change made no difference.

### Most likely causes, in order

1. **NVIDIA kernel module vs. userspace driver version mismatch** (classic
   after a driver upgrade without a reboot, or after a kernel update where
   DKMS didn't rebuild):
   ```
   nvidia-smi
   cat /proc/driver/nvidia/version
   dmesg | grep -i nvidia | tail -50
   ```
   If `nvidia-smi` says "Failed to communicate with the NVIDIA driver", or
   its version differs from `/proc/driver/nvidia/version`, **reboot** (or
   `sudo rmmod nvidia_uvm nvidia_drm nvidia_modeset nvidia && sudo modprobe nvidia`).

2. **GLVND / libGL pointing at the wrong vendor.** A stale
   `/etc/ld.so.conf.d/` entry, leftover Bumblebee/Optimus config, or a
   manually-installed `libGL.so` from an old NVIDIA `.run` installer can make
   `glxinfo` ask the wrong driver for a context:
   ```
   ldconfig -p | grep -E 'libGL\.so|libGLX'
   ls -l /usr/lib/x86_64-linux-gnu/libGL.so*
   __GLX_VENDOR_LIBRARY_NAME=mesa   glxinfo -B
   __GLX_VENDOR_LIBRARY_NAME=nvidia glxinfo -B
   ```
   Whichever vendor variant succeeds tells you which side is healthy.

3. **Wayland / SSH / container without proper GLX.** If `$DISPLAY` is e.g.
   `localhost:10.0` (X forwarding), `BadValue` on `glXCreateNewContext` is
   common because indirect GLX is disabled by default on most X servers.
   Test on the local console, or use `ssh -Y` with `+iglx`, or VirtualGL.

4. **Mesa + kernel DRM mismatch** (rarer). Check
   `dmesg | grep -iE 'drm|i915|amdgpu|nouveau'`.

### Bottom line

Get `glxinfo -B` to print a clean OpenGL version first. Once that works, the
Doppler GLFW error will go away on its own. The `#ifdef __APPLE__` guard
around `GLFW_OPENGL_FORWARD_COMPAT` is still the right change to keep — it
just isn't what's blocking you today.
