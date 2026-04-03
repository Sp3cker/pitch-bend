# AGENTS.md - pitch-bend

Instructions for coding agents working in this repository.


 GUI and platform entry points are C++.

## Read This First

- Prefer small, direct changes, but evaluate the long-term value of refactoring and adding abstractions for architectural purposes and do so if necessary.

- Avoid vendor and generated trees unless the task explicitly requires them.
- Do not add dependencies that require reworking the submodule layout without explicitly calling that out.
- Do not edit vendor code to work around local project bugs unless the task explicitly requires vendor changes.

## Validation Expectations

A task is not complete until all applicable items are done:

1. Build the touched target or targets.
3. Run any additional affected test or executable if the change is integration-sensitive.
4. If validation cannot run, say exactly what was skipped and why.
5. Report any platform caveats or follow-up work.


## Coding Rules

- Match the style of the file you edit. Use tabs and K&R braces.
- Use `snake_case` for functions and variables, `ALL_CAPS` for macros and constants.
- Do not reformat unrelated code. An easy-to-explain git diff is important.

Example style:

```c
static void process_channel(M4aChannel *ch, int16_t *buf, uint32_t count) {
	if (!ch->active) {
		return;
	}
	for (uint32_t i = 0; i < count; i++) {
		buf[i] += render_sample(ch);
	}
}
```


### Frequency Math

- Frequency scaling must match GBA `MidiKeyToFreq()` behavior.
- Use the lookup tables in `plugin/m4a_tables.c` plus the fixed-point multiply path already in the codebase.
- Do not replace this with floating-point approximations.

### Plugin State And Config

- When adding plugin behavior, check existing CLAP extensions before inventing custom mechanisms.

Currently implemented CLAP extensions: `AUDIO_PORTS`, `NOTE_PORTS`, `PARAMS`, `STATE`, `GUI`, `TIMER_SUPPORT`

## Platform Notes

- macOS is the default correctness target.
- If a change touches platform-specific code, note Windows and Linux follow-up work if needed, but do not block macOS completion on them.
- Use existing platform guards such as `__APPLE__` and `_WIN32`.
- The GUI uses Dear ImGui + Pugl with Metal on macOS and OpenGL elsewhere. Do not assume GLFW.

## Known Gaps

Already known — do not treat as surprise regressions unless the task is specifically about them:

- High-DPI or scale-aware window sizing
- File browser dialog for Project Root selection
- Performance profiling and optimization
