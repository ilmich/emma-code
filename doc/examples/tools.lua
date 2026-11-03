-- Example: registering custom tools the model can call.
--
-- Tools are announced to the LLM with the built-in ones (read_file,
-- write_file, list_dir, bash). When the model decides to call one, emma
-- runs the handler and feeds the string result back to the model within
-- the same turn.

-- A safe, read-only tool: no approval needed.
emma.register_tool("word_count", "Count the words of a text",
	{ type = "object",
	  properties = { text = { type = "string" } },
	  required = { "text" } },
	function(args)
		local n = 0
		for _ in args.text:gmatch("%S+") do n = n + 1 end
		return tostring(n)
	end)

-- A side-effectful tool: emma asks the user for confirmation before
-- every execution (same mechanism as the built-in bash tool).
emma.register_tool("touch_file", "Create an empty file if missing",
	{ type = "object",
	  properties = { path = { type = "string" } },
	  required = { "path" } },
	function(args)
		local f = io.open(args.path, "a")
		if f == nil then
			error("cannot open " .. tostring(args.path))
		end
		f:close()
		return "ok"
	end,
	{ requires_approval = true })
