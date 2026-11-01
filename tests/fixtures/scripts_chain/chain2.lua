-- Fixture: second hook of the post_message chain.
emma.register_hook("post_message", function(text)
	return text .. "-B"
end)
