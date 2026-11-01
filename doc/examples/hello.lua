-- Example emma-code script: commands, message hooks and UI access.
-- Copy to ~/.config/emma/scripts/hello.lua and restart emma.

-- Slash command: type "/hello <name>" in emma.
emma.register_command("hello", "greets you warmly", function(args)
	if args == "" then
		args = "world"
	end
	emma.append_message("system", "saying hello to " .. args)
	return "hello, " .. args .. "!"
end)

-- pre_message: fires when you press enter. The returned text replaces
-- the original in the conversation sent to the model.
emma.register_hook("pre_message", function(text)
	-- Example: ask for concise answers without retyping it.
	return text .. " (keep the answer short)"
end)

-- post_message: fires when the model finishes replying. The returned
-- text replaces the original in the UI and in the session history.
emma.register_hook("post_message", function(text)
	return text .. "\n— answered via emma"
end)
