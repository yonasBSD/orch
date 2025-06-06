--
-- Copyright (c) 2024 Kyle Evans <kevans@FreeBSD.org>
--
-- SPDX-License-Identifier: BSD-2-Clause
--

local direct = require("porch.direct")

local generator = {}

local inbuf = ""
local outbuf = ""
local process
local scriptf

-- Escape for double quotes and escape characters
local function escape_arg(str)
	local nstr = ""
	for i = 1, #str do
		local chr = str:sub(i, i)

		if chr == "\\" or chr == "\"" then
			nstr = nstr .. "\\"
		end

		nstr = nstr .. chr
	end

	return nstr
end

-- Escape for double quotes, escape characteres and pattern characters
local function escape_pattern(str)
	local nstr = ""
	for i = 1, #str do
		local chr = str:sub(i, i)

		if chr == "\\" or chr == "\"" then
			nstr = nstr .. "\\"
		elseif chr:match("[-^$()%%%.%[%]*+?]") then
			nstr = nstr .. "%"
		end

		nstr = nstr .. chr
	end

	return nstr
end

local function output(str)
	if not str then
		-- EOF
		return
	end

	outbuf = outbuf .. str
	io.stdout:write(str)
end

local function dump_matches()
	local outlines = {}
	for line in outbuf:gmatch("[^\r\n]+") do
		outlines[#outlines + 1] = line
	end

	local line_count = #outlines
	outbuf = ""
	if line_count > 0 then
		-- We'll output up to 3 lines of context before, just in
		-- case the user actually wanted to swap something out.
		local max_context = 3
		local context = math.min(max_context, line_count - 1)

		if context > 0 then
			scriptf:write("--[[ CONTEXT\n")

			local lower = line_count - context
			for i = lower, line_count - 1 do
				local line = outlines[i]

				scriptf:write("match \"")
				scriptf:write(escape_pattern(line))
				scriptf:write("\"\n")
			end
			scriptf:write("]]\n")
		end

		scriptf:write("match \"")
		scriptf:write(escape_pattern(outlines[line_count]))
		scriptf:write("\"\n")
	end
end

local function input_line(write_line, terminate)
	dump_matches()

	scriptf:write("write \"")
	scriptf:write(escape_arg(write_line:match("[^\r\n]+")))
	if terminate then
		scriptf:write("\\r")
	end
	scriptf:write("\"\n\n")
end

local function input(str)
	local clear_inbuf = false

	local function isprint(val)
		if val >= 0x20 then
			return true
		end

		-- Control characters that we consider printable
		return val == 0x09 or val == 0x0a or val == 0x0d
	end
	if str then
		for i = 1, #str do
			local chr = str:sub(i, i)
			local val = chr:byte(1)

			if isprint(val) then
				inbuf = inbuf .. chr
			else
				-- If we receive a control character, dump output
				-- immediately in case the control character would
				-- result in output.
				inbuf = inbuf .. "^" .. string.char(0x40 + val)

				input_line(inbuf, false)
				clear_inbuf = true
			end
		end
	end

	while inbuf:match("[\r\n]") do
		-- Chop off the next line
		local write_line = inbuf:match("[^\r\n]+[\r\n]+")
		inbuf = inbuf:sub(#write_line + 1)

		input_line(write_line, true)
	end

	if not str then
		-- At EOF, dump anything remaining in the input buffer and
		-- bail out.
		if #inbuf > 0 then
			input_line(inbuf)
		end
		return
	end

	process:write(str)
	if clear_inbuf then
		inbuf = ""
	end
end

-- XXX Remove?
-- We mostly care about config.command here, as we'll record it for spawn(),
-- execute it, then read the output until the user interjects.
function generator.generate_script(scriptfile, config)
	if not config or not config.command then
		error("No command specified")
	end

	process = direct.spawn(table.unpack(config.command))

	io.stdout:setvbuf("no")
	scriptf = assert(io.open(scriptfile, "w"))

	scriptf:write("-- Generated by porchgen(1)\n")
	scriptf:write("spawn(")

	local narg = 1
	for _, arg in pairs(config.command) do
		if narg > 1 then
			scriptf:write(", ")
		end

		scriptf:write("\"", escape_arg(arg), "\"")
		narg = narg + 1
	end

	scriptf:write(")\n\n")
	scriptf:write("release()\n\n")

	return process:proxy(io.stdin, output, input)
end

return generator
