-- Fixture: registers one well-behaved tool, one failing tool and one
-- tool that requires approval.

emma.register_tool("upper", "Uppercase the given text",
	{ type = "object",
	  properties = { text = { type = "string" } },
	  required = { "text" } },
	function(args)
		return string.upper(args.text)
	end)

emma.register_tool("boom", "Always fails",
	{ type = "object", properties = {} },
	function(args)
		error("kaboom")
	end)

emma.register_tool("risky", "Requires user approval",
	{ type = "object", properties = {} },
	function(args)
		return "risky done"
	end,
	{ requires_approval = true })
