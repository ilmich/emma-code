-- Fixture: runtime error during script top-level execution (unknown hook
-- event raises a Lua error); must be reported and skipped.
emma.register_hook("bogus_event", function(text)
	return text
end)
