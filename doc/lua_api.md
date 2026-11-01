# emma-code Lua API reference

Emma-code embeds a Lua 5.4 interpreter (5.3 accepted) as its scripting
engine. At startup it loads every `*.lua` file — in alphabetical order,
skipping hidden files — from the scripts directory configured with
`scripts_dir` in `config.toml`, defaulting to `$XDG_CONFIG_HOME/emma/scripts`
or `~/.config/emma/scripts`.

Scripts receive a single global table, `emma`. All functions report errors
as Lua errors; a script that fails to load or raises an error at top level
is skipped and the error is surfaced in the UI — emma keeps running.

The full Lua standard library is available (including `io` and `os`).
Scripts are plain files on your disk: treat them with the same trust you
give your `config.toml`.

## Commands

### `emma.register_command(name, help, handler)`

Registers a slash command in emma's command registry.

- `name` (string): command name without the leading `/`; spaces, tabs and
  `/` are rejected. Duplicate names (including built-ins like `exit`) are
  rejected with an error.
- `help` (string or nil): one-line help text.
- `handler` (function): called as `handler(args)` where `args` is the text
  following the command name ("" when absent). Its string return value is
  displayed to the user as command feedback; returning `nil` shows no
  feedback. Errors raised by the handler are caught and shown as
  `[lua] command failed`.

```lua
emma.register_command("date", "print the current date", function()
  return os.date("%Y-%m-%d %H:%M")
end)
```

## Message hooks

Hooks let a script observe and rewrite each message in the chat flow.
Register with `emma.register_hook(event, fn)` where `event` is one of:

- `"pre_message"` — fired when the user presses enter, **before** the
  message is stored in the session history or displayed.
- `"post_message"` — fired when the LLM completes its response, **before**
  the response is displayed or stored.

`fn` is called with the message text. The string it returns replaces the
original text everywhere (UI display, session history and, for
`pre_message`, the API request). Returning `nil` leaves the text
unchanged.

Multiple hooks on the same event are chained in registration order: each
receives the previous hook's output. A hook that raises an error is
reported and skipped for that message (pass-through).

```lua
-- Ask the model to always answer concisely
emma.register_hook("pre_message", function(text)
  return text .. " (answer in one sentence)"
end)

-- Append a footer to every assistant reply
emma.register_hook("post_message", function(text)
  return text .. "\n— via emma"
end)
```

Registering a hook for any other event name raises an error.

## Terminal UI

### `emma.append_message(role, content)`

Appends a message line to the terminal UI. `role` is a free-form string;
emma styles `"user"`, `"assistant"` and `"system"` lines.

### `emma.set_status(text)`

Sets the status line; pass `nil` to clear it.

```lua
emma.register_command("hello", "demo UI access", function()
  emma.set_status("hello in progress...")
  emma.append_message("system", "hello from Lua!")
  emma.set_status(nil)
  return "done"
end)
```

## Complete example

See [`examples/hello.lua`](examples/hello.lua) for a commented script that
combines commands, hooks and UI access.

## Notes and limitations

- All scripts run in the same Lua state: they share globals and can see
  each other's registrations.
- Configuration values (`base_url`, `model`, ...) and the API key are not
  exposed to scripts.
- Scripts cannot perform HTTP requests through emma's client in this
  release.
