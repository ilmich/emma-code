-- Fixture used by tests/test_lua_scripting.c: registers a command and
-- both message hooks.
emma.register_command("greet", "greets the user", function(args)
	if args == "" then
		return "hello, stranger"
	end
	return "hello, " .. args
end)

-- Only transforms the exact "hook-me" payload; nil = pass-through.
emma.register_hook("pre_message", function(text)
	if text == "hook-me" then
		return text .. "!"
	end
	return nil
end)

emma.register_hook("post_message", function(text)
	return text .. " [ok]"
end)
