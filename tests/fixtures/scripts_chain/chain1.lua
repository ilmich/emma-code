-- Fixture: first hook of the post_message chain (alphabetical load order).
emma.register_hook("post_message", function(text)
	return text .. "-A"
end)
