# Build, device, and IDE logs

Use the same API for all exported text formats:

```sh
logsonfire analyze exported-log.txt --human
logsonfire analyze exported-log.txt --format logcat
```

| Source | Coverage | Sample under `examples/formats/` |
| --- | --- | --- |
| Xcode | Compiler/linker, signing/provisioning, build/test markers | `xcode.log` |
| Android logcat | Threadtime, brief/time, common Studio copied rows; priorities, tags, PID/TID, exceptions and causes | `android-logcat.log` |
| Android Studio | JetBrains `idea.log` severity/logger headers, exception chains | `android-studio.log` |
| VS Code | Timestamped severity headers, extension activation/host failures | `vscode.log` |
| Make | Missing targets, Makefile recipe errors, compiler/linker failures | `make.log` |
| CMake | Configuration errors and required dependencies, optional-probe handling | `cmake.log` |
| Gradle/Kotlin | Failed tasks, dependency failures, Kotlin source diagnostics | `gradle.log` |

Samples are synthetic. Auto-detection uses recognizable headers and markers;
explicit format labels help with mixed or short exports. Findings retain original
physical line numbers and cleaned text.

## Xcode

```sh
logsonfire-agent run --quiet -- xcodebuild -scheme ExampleApp build
```

Capture the command to retain its exit status, or export plain text from Xcode's
build report. Binary `.xcresult` bundles and compressed `.xcactivitylog` files
require a text export first; LogsOnFire does not decode those containers.

## Android logcat

```sh
adb logcat -d -v threadtime > logcat.txt
logsonfire analyze logcat.txt --format logcat --human
```

The adapter reads logcat priority/tag and timestamp/PID/TID prefixes, following
Android's [logcat reference](https://developer.android.com/tools/logcat).
Structured priorities are authoritative: an informational message mentioning a
handled error does not become an error finding. Repetitions are grouped within
the same PID/TID/tag; changing timestamps alone does not create new groups.
Java stack frames and process headers remain context rather than individual
errors. Nested `Caused by:` evidence is prioritized, with bounded stack context
that excludes other PIDs.

Findings expose `origin.system`, `priority`, `tag`, `pid`, and `tid`. PID/TID
values are strings and are empty where the input format has no such fields.

A device-service error does not itself prove that your app crashed. Inspect its
PID/tag. An error-free excerpt does not establish app health either. A successful
`adb logcat` command only establishes that the dump command completed.

## Android Studio and VS Code

Analyze Studio's `idea.log` or exported Build/Gradle output. Use
`--format android-studio` for a short IDE excerpt. Shared `idea.log` headers may
auto-detect as `jetbrains`; the same severity/logger adapter applies.
Android Studio's [configuration guide](https://developer.android.com/studio/intro/studio-config)
documents log directories and `idea.log.path`.

For VS Code, export the relevant developer log or Output channel, such as the
Extension Host, renderer, or a specific extension. Use `--format vscode` for short
exports without identifying headers. See VS Code's
[process-log troubleshooting](https://code.visualstudio.com/docs/supporting/troubleshoot-terminal-launch)
and [remote-log guidance](https://code.visualstudio.com/docs/remote/troubleshooting).

IDE logs describe continuing sessions. Startup messages and absence of detected
errors do not mark a session `passed`.

## Make, CMake, and Gradle

```sh
logsonfire-agent run --quiet -- make -j8
logsonfire-agent run --quiet -- cmake -S . -B build
logsonfire-agent run --quiet -- cmake --build build --parallel
logsonfire-agent run --quiet -- ./gradlew assembleDebug
```

CMake intentionally fails feature probes and can miss optional packages. Those
lines do not establish a configuration failure. Required dependency details are
interpreted within a nearby `CMake Error` block. `Configuring done` and
`Generating done` do not certify a completed build; capture the specific command
to supply its exit status.

These bounded text adapters do not cover every historical IDE/tool version.
Mixed and custom formats can still require explicit hints or context queries.
