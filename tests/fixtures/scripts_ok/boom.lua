-- Fixture: command whose handler always raises a runtime error; the
-- dispatch must surface the error without crashing.
emma.register_command("boom", nil, function()
	error("kaboom")
end)
