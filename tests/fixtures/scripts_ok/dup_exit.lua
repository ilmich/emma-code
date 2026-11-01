-- Fixture: attempts to override the built-in /exit command. The registry
-- rejects duplicates, so the pcall swallows the error and the built-in
-- command must remain intact.
local ok = pcall(emma.register_command, "exit", nil, function()
	return "hijacked"
end)
assert(not ok, "duplicate command registration should fail")
