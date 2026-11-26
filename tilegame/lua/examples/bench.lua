-- What things cost in Lua on this machine, in microseconds each: every test repeats something n times, timed with millis(), and the time of an
-- empty loop is taken off. n is doubled until the empty loop takes 100 ms, so that the millisecond clock is fine enough. The tests run with the
-- tile screen up; there is a waitframe() between them. It takes about half a minute.

gfx(8)
newmap(40, 30)
p = actor(1, 100, 100)
G = 0
local function f(k) return 0 end
local up = 0
local g = function(k) return up end
local obj = {x = 0}
waitframe()

-- each test is a loop compiled for it, so that its variables are locals, as they would be in a game
local header = "local n, frames, millis, actorx, gettile, tile, actorpos, btn, abs, p, f, g, obj = ... " ..
               "local a, b, x, y, s = 0, 0, 3, 4, '' local t = millis() for i = 1, n do "
local footer = " end return millis() - t"
local function measure(stmt, n)
  waitframe()
  local chunk = assert(load(header .. stmt .. footer))
  return chunk(n, frames, millis, actorx, gettile, tile, actorpos, btn, math.abs, p, f, g, obj)
end

local n, base = 1000, 0
repeat
  n = n * 2
  base = measure("", n)
until base >= 100

local tests = {
  {"a = 0", "a = 0"},
  {"a = a + 1", "a = a + 1"},
  {"a = abs(i)", "a = abs(i)"},
  {"a = frames()", "a = frames()"},
  {"a = actorx(p)", "a = actorx(p)"},
  {"a = gettile(3, 4)", "a = gettile(3, 4)"},
  {"a = btn(0, 16)", "a = btn(0, 16)"},
  {"tile(3, 4, 1)", "tile(3, 4, 1)"},
  {"actorpos(p,100,100)", "actorpos(p, 100, 100)"},
  {"a = f(i)", "a = f(i)"},
  {"a = g(i) closure", "a = g(i)"},
  {"if a==0 then b=1", "if a == 0 then b = 1 end"},
  {"a = x + y * 2", "a = x + y * 2"},
  {"G = G + 1  global", "G = G + 1"},
  {"frames() global", "a = _ENV.frames()"},
  {"o.x = o.x + 1", "obj.x = obj.x + 1"},
  {"s = 'a' .. i", "s = 'a' .. i"},
}

local lines = {}
for _, t in ipairs(tests) do
  local ms = measure(t[2], n)
  lines[#lines + 1] = string.format("  %-20s %7.3f", t[1], (ms - base) * 1000 / n)
end

textmode()
print(string.format("%s, microseconds each (%d repeats, the empty loop taken off)", _VERSION, n))
print(string.format("  %-20s %7.3f", "empty loop turn", base * 1000 / n))
for _, l in ipairs(lines) do print(l) end
