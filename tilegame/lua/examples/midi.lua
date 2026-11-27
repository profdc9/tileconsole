-- midi.lua: a MIDI file (.mid) made into MML for music(): tracks for each part, an instrument for each General MIDI sound, and the drums.
--
--   midi = require("midi")
--   midi.play("song.mid")                  -- plays it (its instruments are made from instrument 1 on)
--   midi.play("song.mid", {loop = true})   -- ... over and over
--   midi.convert("song.mid", "song.lua")   -- writes a program that makes the instruments and plays the MML: RUN it, change it, dofile it
--   song = midi.load("song.mid")           -- the MML for a program to use: song.tracks (strings), song.instruments, song.summary
--
-- Options, in a table after the file name:
--   voices      how many tracks at most (1 to 16; 12 if not given), so the voices before them stay free for the game's sounds. A track is a
--               voice and plays one note at a time, so a chord takes a track for each of its notes. When the music needs more, the parts
--               that play least give up tracks first; a note that then finds no free track cuts off the one that began longest ago, or is
--               left out if they all began with it
--   loop        true: the music starts again at its end (the tracks are made as long as each other, to the end of the last bar)
--   grid        where notes may start and end, as divisions of a whole note: {32, 24} if not given (32nd notes, and 16th-note triplets);
--               one number for one grid (16: 16th notes), or false to keep the times as they are (to a 96th of a quarter note). A file
--               played in by hand needs a grid; one written note by note fits any grid as fine as its shortest notes
--   transpose   semitones up (down if negative), for the notes but not the drums
--   drums       false leaves out channel 10 (General MIDI's drums)
--   channels    only these MIDI channels (1 to 16): {1, 2, 10}
--   pedal       true: the sustain pedal holds notes on (which takes more tracks)
--   instrument  the first instrument number to use (1 if not given)
--   first       for play and convert: the voice of the first track (as music's first argument); the last voices if not given
--   notes       true: load's song keeps song.lines, the notes of each track (as tests want them; it is a lot of memory)
--
-- What is lost: pitch bends, the sound of each instrument (an envelope and a wave stand in for it: see FAMILY and PRESET below), volume
-- changes in the middle of a note (they come in at the next note), and a tempo change that falls inside a note of every track, which
-- is moved to the nearest place where one starts or ends.

local M = {}

local NAMES = {
  "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2",
  "Harpsichord", "Clavinet", "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
  "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
  "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)", "Electric Guitar (jazz)", "Electric Guitar (clean)", "Electric Guitar (muted)",
  "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics", "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)",
  "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2", "Violin", "Viola", "Cello", "Contrabass",
  "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani", "String Ensemble 1", "String Ensemble 2", "Synth Strings 1",
  "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit", "Trumpet", "Trombone", "Tuba", "Muted Trumpet",
  "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2", "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe",
  "English Horn", "Bassoon", "Clarinet", "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
  "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)", "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)",
  "Lead 8 (bass + lead)", "Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)",
  "Pad 7 (halo)", "Pad 8 (sweep)", "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)",
  "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)", "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
  "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal", "Guitar Fret Noise",
  "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause", "Gunshot",
}

-- The instrument for each family of eight General MIDI programs (0 the pianos, 1 the bells, ... 15 the sound effects), as instrument()
-- takes it; PRESET has the programs that sound unlike their family. The volume is worked out for each song.
local FAMILY = {
  [0] = {"pulse", {duty = 100, decay = 900, sustain = 70, release = 200}},   -- pianos
  {"wave", {wave = "sine", decay = 700, sustain = 0, release = 300}},        -- celesta, glockenspiel, marimba ...
  {"pulse", {attack = 5, release = 60}},                                     -- organs
  {"pulse", {duty = 72, decay = 700, sustain = 50, release = 150}},          -- guitars
  {"triangle", {decay = 400, sustain = 170, release = 60}},                  -- basses
  {"wave", {wave = "saw", attack = 80, sustain = 230, release = 250}},       -- strings
  {"wave", {wave = "saw", attack = 120, sustain = 220, release = 300}},      -- string ensembles
  {"pulse", {duty = 96, attack = 30, decay = 200, sustain = 200, release = 100}},   -- brass
  {"pulse", {duty = 56, attack = 25, sustain = 220, release = 80}},          -- saxophones, oboe, clarinet
  {"wave", {wave = "sine", attack = 40, sustain = 230, release = 100}},      -- flutes
  {"pulse", {sustain = 220, release = 60}},                                  -- synth leads
  {"wave", {wave = "triangle", attack = 250, sustain = 220, release = 500}}, -- synth pads
  {"wave", {wave = "sine", attack = 100, sustain = 200, release = 400}},     -- synth effects
  {"pulse", {duty = 64, decay = 600, sustain = 40, release = 150}},          -- sitar, banjo, koto ...
  {"triangle", {decay = 300, sustain = 0, release = 50}},                    -- steel drums, woodblock, taiko ...
  {"noise", {decay = 500, sustain = 0, release = 100}},                      -- sound effects
}

local function same(n, ...) local t = {} for _, p in ipairs({...}) do t[p] = n end return t end
local PRESET = {}
for p, v in pairs(same({"wave", {wave = "sine", decay = 1200, sustain = 90, release = 250}}, 4, 5)) do PRESET[p] = v end   -- electric pianos
for p, v in pairs(same({"pulse", {duty = 64, attack = 30, sustain = 230, release = 80}}, 21, 22, 23)) do PRESET[p] = v end   -- accordions
for p, v in pairs(same({"pulse", {decay = 2000, sustain = 180, release = 100}}, 29, 30)) do PRESET[p] = v end   -- overdriven guitars
for p, v in pairs(same({"pulse", {duty = 96, decay = 300, sustain = 150, release = 60}}, 38, 39)) do PRESET[p] = v end   -- synth basses
for p, v in pairs(same({"wave", {wave = "sine", attack = 150, sustain = 220, release = 300}}, 52, 53, 54)) do PRESET[p] = v end   -- voices
for p, v in pairs(same({"triangle", {attack = 40, sustain = 230, release = 100}}, 72, 73, 74)) do PRESET[p] = v end   -- piccolo, flute
PRESET[6] = {"pulse", {duty = 48, decay = 500, sustain = 40, release = 80}}     -- harpsichord
PRESET[7] = {"pulse", {duty = 40, decay = 400, sustain = 60, release = 60}}     -- clavinet
PRESET[11] = {"wave", {wave = "sine", decay = 1200, sustain = 40, release = 400}}   -- vibraphone
PRESET[14] = {"wave", {wave = "sine", decay = 2000, sustain = 0, release = 600}}    -- tubular bells
PRESET[45] = {"triangle", {decay = 250, sustain = 0, release = 50}}             -- pizzicato
PRESET[46] = {"triangle", {decay = 900, sustain = 0, release = 200}}            -- harp
PRESET[47] = {"triangle", {decay = 500, sustain = 0, release = 100}}            -- timpani
PRESET[55] = {"pulse", {decay = 300, sustain = 0, release = 50}}                -- orchestra hit
PRESET[81] = {"wave", {wave = "saw", sustain = 220, release = 60}}              -- sawtooth lead
PRESET[109] = {"pulse", {duty = 48, sustain = 230, release = 80}}               -- bagpipe

-- A sound for each kind of drum: the instrument, its note (for the noises how fast they are clocked; the toms and bells take theirs from
-- DRUMS), and which is kept first when two hit at once and there is one track for them.
local KIT = {
  kick = {"noise", 60, {decay = 80, sustain = 0}, 1},
  snare = {"noise", 120, {decay = 150, sustain = 0}, 2},
  hat = {"buzz", 127, {decay = 30, sustain = 0}, 3},
  crash = {"noise", 127, {decay = 800, sustain = 0}, 4},
  tom = {"triangle", 50, {decay = 220, sustain = 0}, 5},
  clap = {"noise", 115, {decay = 120, sustain = 0}, 5},
  ride = {"buzz", 122, {decay = 350, sustain = 0}, 6},
  stick = {"buzz", 110, {decay = 40, sustain = 0}, 6},
  openhat = {"buzz", 127, {decay = 220, sustain = 0}, 7},
  shaker = {"noise", 127, {decay = 45, sustain = 0}, 8},
  cowbell = {"pulse", 79, {duty = 64, decay = 150, sustain = 0}, 8},
  block = {"triangle", 84, {decay = 60, sustain = 0}, 8},
  bell = {"wave", 96, {wave = "sine", decay = 500, sustain = 0}, 8},
}
-- General MIDI's drum notes (channel 10): the kind, and the note for the ones with a pitch. Any other is a snare.
local DRUMS = {
  [35] = {"kick"}, [36] = {"kick"}, [37] = {"stick"}, [38] = {"snare"}, [39] = {"clap"}, [40] = {"snare"}, [41] = {"tom", 43},
  [42] = {"hat"}, [43] = {"tom", 47}, [44] = {"hat"}, [45] = {"tom", 50}, [46] = {"openhat"}, [47] = {"tom", 53}, [48] = {"tom", 57},
  [49] = {"crash"}, [50] = {"tom", 60}, [51] = {"ride"}, [52] = {"crash"}, [53] = {"ride"}, [54] = {"shaker"}, [55] = {"crash"},
  [56] = {"cowbell"}, [57] = {"crash"}, [58] = {"shaker"}, [59] = {"ride"}, [60] = {"tom", 72}, [61] = {"tom", 67}, [62] = {"tom", 65},
  [63] = {"tom", 62}, [64] = {"tom", 58}, [65] = {"tom", 55}, [66] = {"tom", 52}, [67] = {"bell", 84}, [68] = {"bell", 79},
  [69] = {"shaker"}, [70] = {"shaker"}, [71] = {"bell", 96}, [72] = {"bell", 91}, [73] = {"shaker"}, [74] = {"shaker"},
  [75] = {"block", 88}, [76] = {"block", 84}, [77] = {"block", 79}, [78] = {"block", 74}, [79] = {"block", 69}, [80] = {"bell", 100},
  [81] = {"bell", 100},
}

-- How loud a full-volume note of each wave sounds next to a square wave's, so the parts come out about as loud as the file has them.
local LOUDNESS = {pulse = 1, noise = 1, buzz = 1, triangle = 1.5, sine = 1.35, saw = 1.4, square = 1}

local WHOLE = 384    -- ticks in a whole note, as MML has them (96 a quarter)
local NOTE_NAMES = {"c", "c+", "d", "d+", "e", "f", "f+", "g", "g+", "a", "a+", "b"}

local function copy(t) local c = {} for k, v in pairs(t) do c[k] = v end return c end

-- MML lengths: SINGLE[ticks] is the shortest way to write a length as one number with its dots ("8.", "3"); SIZES has them longest first.
local SINGLE, SIZES = {}, {}
for _, n in ipairs({1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 384}) do
  local len, add, text = WHOLE // n, WHOLE // n, tostring(n)
  for dots = 0, 3 do
    if dots > 0 then
      if add % 2 == 1 then break end
      add = add // 2
      len = len + add
      text = text .. "."
    end
    if not SINGLE[len] or #text < #SINGLE[len] then SINGLE[len] = text end
  end
end
for len in pairs(SINGLE) do SIZES[#SIZES + 1] = len end
table.sort(SIZES, function(a, b) return a > b end)

-- A length as the parts tied with ^ that make it: one if it can be, then two, then three; failing those %ticks.
local PARTS = {}
local function parts(len)
  if PARTS[len] then return PARTS[len] end
  local p
  if SINGLE[len] then
    p = {SINGLE[len]}
  else
    for _, a in ipairs(SIZES) do
      if a < len and SINGLE[len - a] then p = {SINGLE[a], SINGLE[len - a]} break end
    end
    if not p then
      for _, a in ipairs(SIZES) do
        if a < len then
          for _, b in ipairs(SIZES) do
            if b < len - a and SINGLE[len - a - b] then p = {SINGLE[a], SINGLE[b], SINGLE[len - a - b]} break end
          end
          break
        end
      end
    end
    p = p or {"%" .. len}
  end
  PARTS[len] = p
  return p
end

------------------------------------------------------------------------------------------------------------------------------------------
-- Reading the file

local function fail(msg) error("midi: " .. msg, 0) end

-- The events of every track, in the order they happen: T[i] the time in the file's ticks, E[i] what it is (packed: track, kind, channel,
-- two data bytes), at a time note-offs first, then everything else, then note-ons. Tempo changes and the first time signature apart.
local function read_midi(data, name)
  local byte = string.byte
  if data:sub(1, 4) ~= "MThd" or #data < 14 then fail(name .. " is not a MIDI file") end
  local function u32(i) local a, b, c, d = byte(data, i, i + 3) return ((a * 256 + b) * 256 + c) * 256 + d end
  local function u16(i) local a, b = byte(data, i, i + 1) return a * 256 + b end
  local division = u16(13)
  local song = {T = {}, E = {}, tempo = {}, names = {}, division = division}
  if division >= 0x8000 then   -- SMPTE: ticks a second (frames a second times ticks a frame), as if it were one quarter note a second
    song.division = (256 - (division >> 8)) * (division & 255)
    song.smpte = true
  end
  if song.division <= 0 then fail(name .. ": the header's division is 0") end
  local T, E, n = song.T, song.E, 0
  local pos, track = 9 + u32(5), 0
  while pos + 7 <= #data do
    local id, len = data:sub(pos, pos + 3), u32(pos + 4)
    if len < 0 then break end
    local p, stop = pos + 8, math.min(pos + 7 + len, #data)
    pos = pos + 8 + len
    if id == "MTrk" then
      track = math.min(track + 1, 255)
      local t, status = 0, 0
      local function vlq()
        local v, c = 0, 128
        while c >= 128 and p <= stop do
          c = byte(data, p)
          p = p + 1
          v = v * 128 + (c & 127)
        end
        return v
      end
      while p <= stop do
        t = t + vlq()
        local c = byte(data, p)
        if not c then break end
        if c >= 0x80 then
          p = p + 1
          if c < 0xF0 then status = c end
        else
          c = status   -- running status: the data bytes of another event like the last
        end
        if c == 0xFF then
          local kind = byte(data, p) or 0
          p = p + 1
          local size = vlq()
          if kind == 0x2F then break end
          if kind == 0x51 and size == 3 then
            local a, b, d = byte(data, p, p + 2)
            song.tempo[#song.tempo + 1] = {t = t, us = (a * 256 + b) * 256 + d, i = #song.tempo}
          elseif kind == 0x58 and size >= 2 and (not song.sig or t < song.sig.t) then
            song.sig = {t = t, num = byte(data, p) or 4, den = 1 << math.min(byte(data, p + 1) or 2, 9)}
          elseif kind == 0x03 and not song.names[track] then
            song.names[track] = data:sub(p, p + size - 1):gsub("[%c]", ""):gsub("^%s+", ""):gsub("%s+$", "")
          end
          p = p + size
        elseif c == 0xF0 or c == 0xF7 then
          p = p + vlq()
        elseif c >= 0x80 then
          local hi, ch = c >> 4, c & 15
          local a, b = (byte(data, p) or 0) & 127, 0
          p = p + 1
          if hi ~= 0xC and hi ~= 0xD then
            b = (byte(data, p) or 0) & 127
            p = p + 1
          end
          n = n + 1
          T[n] = t
          E[n] = (((track * 16 + hi) * 16 + ch) * 128 + a) * 128 + b
        else
          p = p + 1   -- a data byte with nothing before it to belong to
        end
      end
    end
  end
  if n == 0 then fail(name .. " has no notes") end
  -- song.order: the events in time order; at one time the note-offs come first, the note-ons last, and the rest as they were in the file
  local order = {}
  for i = 1, n do
    if T[i] >= (1 << 28) then fail(name .. " is too long") end
    order[i] = i
  end
  local function key(i)
    local e = E[i]
    local hi = (e >> 18) & 15
    return T[i] * 4 + ((hi == 8 or (hi == 9 and e & 127 == 0)) and 0 or (hi == 9 and 2 or 1))
  end
  table.sort(order, function(i, j) local a, b = key(i), key(j) if a ~= b then return a < b end return i < j end)
  song.order, song.count = order, n
  table.sort(song.tempo, function(a, b) if a.t ~= b.t then return a.t < b.t end return a.i < b.i end)
  return song
end

-- The notes, from the events: for each, its channel, key, start and end (in the file's ticks), how loud (velocity x channel volume x
-- expression), the program and the pan at its start, and its track (for the part's name).
local function notes_of(song, pedal)
  local prog, vol, expr, pan, held, down = {}, {}, {}, {}, {}, {}
  for ch = 0, 15 do prog[ch], vol[ch], expr[ch], pan[ch], held[ch], down[ch] = 0, 100, 127, 64, {}, false end
  local open, notes = {}, {}
  local function finish(nt, t)
    nt.e0 = t
    if pedal and down[nt.ch] then
      held[nt.ch][#held[nt.ch] + 1] = nt
    else
      notes[#notes + 1] = nt
    end
  end
  local T, E, order, last = song.T, song.E, song.order, 0
  for o = 1, song.count do
    local i = order[o]
    local t, e = T[i], E[i]
    local b, a = e & 127, (e >> 7) & 127
    local ch, hi, track = (e >> 14) & 15, (e >> 18) & 15, e >> 22
    last = t
    if hi == 9 and b > 0 then
      local k = ch * 128 + a
      open[k] = open[k] or {}
      table.insert(open[k], {ch = ch, key = a, s0 = t, e0 = t, loud = b * vol[ch] * expr[ch], prog = prog[ch], pan = pan[ch], track = track,
                             s = 0, e1 = 0, e = 0, p = 0, rank = 0, kit = false, inst = 0, v = 0})   -- (all its fields at once: no growing)
    elseif hi == 8 or hi == 9 then
      local list = open[ch * 128 + a]
      if list and #list > 0 then finish(table.remove(list, 1), t) end
    elseif hi == 0xC then
      prog[ch] = a
    elseif hi == 0xB then
      if a == 7 then vol[ch] = b
      elseif a == 11 then expr[ch] = b
      elseif a == 10 then pan[ch] = b
      elseif a == 64 then
        down[ch] = b >= 64
        if not down[ch] then
          for _, nt in ipairs(held[ch]) do nt.e0 = t notes[#notes + 1] = nt end
          held[ch] = {}
        end
      elseif a == 121 then expr[ch] = 127
      elseif a == 123 then
        for k, list in pairs(open) do
          if k // 128 == ch then for _, nt in ipairs(list) do finish(nt, t) end open[k] = {} end
        end
      end
    end
  end
  for _, list in pairs(open) do for _, nt in ipairs(list) do nt.e0 = last notes[#notes + 1] = nt end end
  for ch = 0, 15 do for _, nt in ipairs(held[ch]) do nt.e0 = last notes[#notes + 1] = nt end end
  return notes
end

-- A function from the file's ticks to MML's (96 a quarter note): q(t, channel) is on the nearest point of a grid; and the smallest step of
-- the grids. With more than one grid, each quarter note of each channel has the one its notes' starts and ends fit best, so that the
-- sixteenths of one beat and the triplets of the next do not mix into lengths of 4 ticks.
local function quantizer(division, grid, notes)
  if grid == nil then grid = {32, 24} end
  if type(grid) == "number" then grid = {grid} end
  local steps = {}
  if grid then
    for _, g in ipairs(grid) do
      if math.type(g) ~= "integer" or g < 4 or WHOLE % g ~= 0 or 96 % (WHOLE // g) ~= 0 then
        fail("the grid is a division of a whole note, 4 or finer: 4 8 16 32 64 96, or 12 24 48 for triplets")
      end
      steps[#steps + 1] = WHOLE // g
    end
  end
  table.sort(steps, function(a, b) return a > b end)
  local function snap(x, g)   -- x: in a quarter note, in 96ths of a division; the nearest multiple of g ticks, and how far it is
    local dg = division * g
    local c = (2 * x + dg) // (2 * dg) * g
    return c, math.abs(c * division - x)
  end
  local choice = {}   -- [channel][quarter note]: the grid's step
  if #steps > 1 then
    local errs = {}
    local function add(ch, t)
      local beat = t // division
      errs[ch] = errs[ch] or {}
      local e = errs[ch][beat]
      if not e then e = {} for i = 1, #steps do e[i] = 0 end errs[ch][beat] = e end
      for i, g in ipairs(steps) do
        local _, d = snap(t % division * 96, g)
        e[i] = e[i] + d
      end
    end
    for _, nt in ipairs(notes) do
      add(nt.ch, nt.s0)
      if nt.ch ~= 9 then add(nt.ch, nt.e0) end
    end
    for ch, beats in pairs(errs) do
      choice[ch] = {}
      for beat, e in pairs(beats) do
        local best = 1
        for i = 2, #steps do if e[i] < e[best] then best = i end end
        choice[ch][beat] = steps[best]
      end
    end
  end
  return function(t, ch)
    local q, r = t // division, t % division
    local x = r * 96
    if #steps == 0 then return q * 96 + (2 * x + division) // (2 * division) end
    local g = choice[ch] and choice[ch][q]
    if g then return q * 96 + snap(x, g) end
    local best, err
    for _, g in ipairs(steps) do
      local c, d = snap(x, g)
      if not err or d < err then best, err = c, d end
    end
    return q * 96 + best
  end, steps[#steps] or 1
end

------------------------------------------------------------------------------------------------------------------------------------------
-- Tracks

-- Puts each note on a line (a track to be), the first that is free at its start (a drum: the first free one that last had the same kind
-- of drum, so that its instrument need not change, or else the first free one); with no free one, a new line if there are fewer than
-- `cap`, or the line whose note began longest ago (that note is cut off), or none (the note is left out). Notes are in order of their
-- starts, and at one start the highest first (for drums the most needed): so the tune keeps its first line.
local function allocate(notes, cap)
  local lines, dropped = {}, 0
  for _, nt in ipairs(notes) do nt.e = nt.e1 end
  for _, nt in ipairs(notes) do
    local pick
    for _, line in ipairs(lines) do
      if line.e <= nt.s then
        pick = pick or line
        if not nt.kit or line.notes[#line.notes].kit == nt.kit then pick = line break end   -- a drum keeps to the line it had
      end
    end
    if not pick then
      if #lines < cap then
        pick = {e = 0, notes = {}}
        lines[#lines + 1] = pick
      else
        local start
        for _, line in ipairs(lines) do
          local last = line.notes[#line.notes]
          if last.s < nt.s and (not start or last.s < start) then pick, start = line, last.s end
        end
        if pick then pick.notes[#pick.notes].e = nt.s end
      end
    end
    if pick then
      pick.notes[#pick.notes + 1] = nt
      pick.e = nt.e
    else
      dropped = dropped + 1
    end
  end
  return lines, dropped
end

-- How much a line plays: what is lost if it goes.
local function weight(line)
  local w = 0
  for _, nt in ipairs(line.notes) do w = w + math.min(nt.e - nt.s, 192) + 48 end
  return w
end

------------------------------------------------------------------------------------------------------------------------------------------
-- Writing a track's MML

local function write_line(line, bar, finish, head)
  local out = {}
  local function put(s) out[#out + 1] = s end
  -- the default length (l): the commonest that is one number
  local count, deflen, most = {}, "4", 0
  local t = 0
  for _, nt in ipairs(line.notes) do
    for _, len in ipairs({nt.s - t, nt.e - nt.s}) do
      if len > 0 and SINGLE[len] then
        count[len] = (count[len] or 0) + 1
        if count[len] > most then most, deflen = count[len], SINGLE[len] end
      end
    end
    t = nt.e
  end
  put(head .. "l" .. deflen)
  local function length(len, comma)
    local p, s = parts(len), ""
    for i, part in ipairs(p) do
      s = s .. (i > 1 and "^" or (comma and part ~= deflen and "," or "")) .. (part == deflen and "" or part)
    end
    return s
  end
  local marks, mi, last_bar, bars = line.marks, 1, 0, 0
  local function at(t)   -- the bar line and the tempo changes at t, if any
    if t > 0 and t % bar == 0 and t ~= last_bar then
      last_bar, bars = t, bars + 1
      put(bars % 4 == 0 and "|\n" or "|")
    end
    while marks[mi] and marks[mi].t <= t do
      put(marks[mi].text)
      mi = mi + 1
    end
  end
  local function rest(a, b)
    local t = a
    while t < b do
      at(t)
      local c = math.min((t // bar + 1) * bar, b)
      if marks[mi] and marks[mi].t > t and marks[mi].t < c then c = marks[mi].t end
      put("r" .. length(c - t))
      t = c
    end
  end
  local octave, inst, vol, pan = nil, nil, nil, 0
  t = 0
  for _, nt in ipairs(line.notes) do
    if nt.s > t then rest(t, nt.s) end
    at(nt.s)
    if nt.inst ~= inst then inst = nt.inst put("@" .. inst) end
    if nt.v ~= vol then vol = nt.v put("v" .. vol) end
    if nt.pan ~= pan then pan = nt.pan put("p" .. pan) end
    local name, comma
    if line.drum or nt.p < 12 then
      name, comma = "n" .. nt.p, true
    else
      local o = nt.p // 12 - 1
      name = NOTE_NAMES[nt.p % 12 + 1]
      if o ~= octave then
        name = (octave and o == octave + 1 and ">" or octave and o == octave - 1 and "<" or "o" .. o .. " ") .. name
        octave = o
      end
    end
    local a = nt.s
    while a < nt.e do
      local c = math.min((a // bar + 1) * bar, nt.e)
      if a == nt.s then
        put(name .. length(c - a, comma))
      else
        at(a)
        put("^" .. length(c - a))
      end
      a = c
    end
    t = nt.e
  end
  if marks[#marks] and marks[#marks].t > finish then finish = marks[#marks].t end   -- a tempo change after the last note
  if finish > t then rest(t, finish) t = finish end
  at(t)
  return table.concat(out, " ")
end

------------------------------------------------------------------------------------------------------------------------------------------

-- The MIDI file `name` as MML: a table with tracks (MML strings, one a voice), instruments ({n =, kind =, settings =, name =} for
-- instrument()), parts (what each track plays), notes, dropped, seconds and summary.
function M.load(name, opt)
  opt = opt or {}
  local f, err = io.open(name, "rb")
  if not f then fail(tostring(err)) end
  local data = f:read("a")
  f:close()
  local song = read_midi(data, name)
  data = nil
  local voices = math.max(1, math.min(16, opt.voices or 12))
  local keep = {}
  if opt.channels then
    for _, c in ipairs(opt.channels) do keep[c - 1] = true end
  else
    for c = 0, 15 do keep[c] = true end
  end
  if opt.drums == false then keep[9] = nil end
  local transpose = opt.transpose or 0

  -- the notes on our ticks, by channel; the drums each with their kind's note
  local all = {}
  for _, nt in ipairs(notes_of(song, opt.pedal)) do
    if keep[nt.ch] then all[#all + 1] = nt end
  end
  song.T, song.E, song.order = nil, nil, nil   -- (the events' memory can go)
  local q, step = quantizer(song.division, opt.grid, all)
  local by = {}
  for _, nt in ipairs(all) do
    do
      nt.s = q(nt.s0, nt.ch)
      if nt.ch == 9 then
        local d = DRUMS[nt.key] or {"snare"}
        nt.kit, nt.p, nt.rank = d[1], d[2] or KIT[d[1]][2], KIT[d[1]][4]
        nt.e1 = nt.s + step
      else
        local p = nt.key + transpose
        while p < 0 do p = p + 12 end
        while p > 127 do p = p - 12 end
        nt.p, nt.rank = p, -p
        nt.e1 = math.max(q(nt.e0, nt.ch), nt.s + step)
      end
      by[nt.ch] = by[nt.ch] or {}
      table.insert(by[nt.ch], nt)
    end
  end
  local groups = {}
  for ch = 0, 15 do
    local list = by[ch]
    if list then
      table.sort(list, function(a, b) if a.s ~= b.s then return a.s < b.s end if a.rank ~= b.rank then return a.rank < b.rank end return a.s0 < b.s0 end)
      local kept, prev = {}, nil   -- the same note twice at once: one, the longer and louder
      for _, nt in ipairs(list) do
        if prev and prev.s == nt.s and prev.p == nt.p and prev.kit == nt.kit then
          prev.e1, prev.loud = math.max(prev.e1, nt.e1), math.max(prev.loud, nt.loud)
        else
          kept[#kept + 1] = nt
          prev = nt
        end
      end
      groups[#groups + 1] = {ch = ch, notes = kept, drum = ch == 9}
    end
  end
  if #groups == 0 then fail(name .. " has no notes on the channels asked for") end
  -- drums last: the tune and the bass are the first tracks
  table.sort(groups, function(a, b) if a.drum ~= b.drum then return b.drum end return a.ch < b.ch end)

  -- as many lines as each part needs; then, while there are too many, the part whose last line plays least loses that line
  local total, left_out = 0, 0
  for _, g in ipairs(groups) do
    g.lines, g.dropped = allocate(g.notes, 16)
    total = total + #g.lines
  end
  while total > voices do
    local pick, least
    for _, g in ipairs(groups) do
      if #g.lines > 1 then
        local w = weight(g.lines[#g.lines])
        if not least or w < least then pick, least = g, w end
      end
    end
    if pick then
      pick.lines, pick.dropped = allocate(pick.notes, #pick.lines - 1)
    else   -- a line each, and still too many: the part that plays least is left out
      local at
      for i, g in ipairs(groups) do
        local w = weight(g.lines[1])
        if not least or w < least then at, least = i, w end
      end
      left_out = left_out + #groups[at].notes
      table.remove(groups, at)
    end
    total = 0
    for _, g in ipairs(groups) do total = total + #g.lines end
  end

  -- the lines in track order, and where the music ends
  local lines, finish, dropped = {}, 0, left_out
  for _, g in ipairs(groups) do
    dropped = dropped + g.dropped
    for _, line in ipairs(g.lines) do
      line.drum, line.ch, line.marks = g.drum, g.ch, {}
      lines[#lines + 1] = line
      for _, nt in ipairs(line.notes) do finish = math.max(finish, nt.e) end
    end
  end
  local bar = WHOLE
  if song.sig and song.sig.num > 0 and WHOLE % song.sig.den == 0 then bar = song.sig.num * WHOLE // song.sig.den end
  if bar <= 0 then bar = WHOLE end
  if opt.loop then finish = (finish + bar - 1) // bar * bar end
  -- a drum sounds until the next on its line: its instrument's envelope ends it (the last one has a half note, or to the end)
  for _, line in ipairs(lines) do
    if line.drum then
      for i, nt in ipairs(line.notes) do
        local nx = line.notes[i + 1]
        nt.e = nx and nx.s or (opt.loop and finish or math.max(finish, nt.s + WHOLE // 2))
      end
    end
  end

  -- instruments: one for each program the notes use (and each kind of drum), from instrument `base` on
  local base = opt.instrument or 1
  local slots, instruments = {}, {}
  local function slot(key, kind, settings, desc)
    if not slots[key] then
      local n = base + #instruments
      if n > 31 then fail(name .. " needs more instruments than there are from " .. base .. " to 31") end
      slots[key] = n
      instruments[#instruments + 1] = {n = n, kind = kind, settings = copy(settings), name = desc}
    end
    return slots[key]
  end
  local programs = {}
  for _, line in ipairs(lines) do for _, nt in ipairs(line.notes) do if not line.drum then programs[nt.prog] = true end end end
  local distinct = 0
  for _ in pairs(programs) do distinct = distinct + 1 end
  local by_family = distinct + 13 > 32 - base   -- too many to have one each: one for each family
  local loudest = 1
  for _, line in ipairs(lines) do
    for _, nt in ipairs(line.notes) do
      loudest = math.max(loudest, nt.loud)
      if line.drum then
        local k = KIT[nt.kit]
        nt.inst = slot("drum " .. nt.kit, k[1], k[3], "drums: " .. nt.kit)
      else
        local pr = by_family and nt.prog // 8 * 8 or nt.prog
        local preset = (not by_family and PRESET[pr]) or FAMILY[pr // 8]
        nt.inst = slot(pr, preset[1], preset[2], by_family and NAMES[pr + 1] .. " and the rest of its family" or NAMES[pr + 1])
      end
    end
  end
  -- the volumes: the loudest note v15; the instruments' so that the most notes at once, all at v15, come to about full scale (pulse waves
  -- are flat at the top, so a few at once add up to their sum: more would be clipped)
  local edges = {}
  for _, line in ipairs(lines) do
    for _, nt in ipairs(line.notes) do
      edges[#edges + 1] = nt.s * 2 + 1
      edges[#edges + 1] = math.min(nt.e, line.drum and nt.s + 48 or nt.e) * 2
    end
  end
  table.sort(edges)
  local now, most = 0, 1
  for _, e in ipairs(edges) do
    now = now + (e % 2 == 1 and 1 or -1)
    most = math.max(most, now)
  end
  local level = math.max(16, math.min(255, 255 // most))
  for _, ins in ipairs(instruments) do
    local w = LOUDNESS[ins.kind == "wave" and ins.settings.wave or ins.kind] or 1
    ins.settings.volume = math.min(255, math.floor(level * w + 0.5))
  end
  for _, line in ipairs(lines) do
    local v   -- a track's volume changes when the notes are louder or softer by 2 or more: what a player's hand varies is left out
    for _, nt in ipairs(line.notes) do
      local loud = math.max(1, math.min(15, (30 * nt.loud + loudest) // (2 * loudest)))
      if not v or math.abs(loud - v) >= 2 then v = loud end
      nt.v = v
      nt.pan = math.max(-128, math.min(127, (nt.pan - 64) * 2))
    end
  end

  -- the tempo: at the start on the first track, a change on the track that has a place for it nearest its time
  local marks = {{t = 0, bpm = 120}}
  if not song.smpte then
    for _, tc in ipairs(song.tempo) do
      local t, bpm = q(tc.t), math.max(20, math.min(600, math.floor(60000000 / math.max(1, tc.us) + 0.5)))
      if t == marks[#marks].t then marks[#marks].bpm = bpm
      elseif bpm ~= marks[#marks].bpm then marks[#marks + 1] = {t = t, bpm = bpm} end
    end
  else
    marks[1].bpm = 60
  end
  local tempos, upto = marks, {}
  for i, m in ipairs(marks) do
    local into, at = lines[1], 0
    if i > 1 then
      local best
      for li, line in ipairs(lines) do
        local k, notes = upto[li] or 1, line.notes   -- the first note that ends after m.t (the notes are in order and do not overlap)
        while notes[k] and notes[k].e <= m.t do k = k + 1 end
        upto[li] = k
        local nt, where, d = notes[k], m.t, 0
        if nt and nt.s < m.t then
          if m.t - nt.s <= nt.e - m.t then where, d = nt.s, m.t - nt.s else where, d = nt.e, nt.e - m.t end
        end
        if not best or d < best then best, into, at = d, line, where end
      end
    end
    into.marks[#into.marks + 1] = {t = at, text = "t" .. m.bpm, i = i}
  end
  for _, line in ipairs(lines) do
    table.sort(line.marks, function(a, b) if a.t ~= b.t then return a.t < b.t end return a.i < b.i end)
  end

  -- the MML, and what each track is
  local tracks, partnames, kept = {}, {}, 0
  for i, line in ipairs(lines) do
    tracks[i] = write_line(line, bar, opt.loop and finish or 0, opt.loop and "L " or "")
    kept = kept + #line.notes
    local first = line.notes[1]
    local what = line.drum and "drums" or NAMES[first.prog + 1]
    local tname = song.names[first.track]
    partnames[i] = ("channel %d, %s%s"):format(line.ch + 1, what, tname and tname ~= "" and (" (" .. tname .. ")") or "")
  end
  local seconds = 0
  for i, m in ipairs(tempos) do
    local upto = tempos[i + 1] and tempos[i + 1].t or finish
    if upto > m.t then seconds = seconds + (upto - m.t) / 96 * 60 / m.bpm end
  end
  local function count(n, what) return n .. " " .. what .. (n == 1 and "" or "s") end
  local summary = ("%s: %s, %s, %d:%02d"):format(name, count(#tracks, "track"), count(kept, "note"), math.floor(seconds) // 60, math.floor(seconds) % 60)
  if dropped > 0 then summary = summary .. (", %s left out (more at once than %s)"):format(count(dropped, "note"), count(voices, "track")) end
  return {tracks = tracks, instruments = instruments, parts = partnames, notes = kept, dropped = dropped, seconds = seconds,
          summary = summary, lines = opt.notes and lines or nil}
end

-- Makes the song's instruments.
function M.instruments(song)
  for _, ins in ipairs(song.instruments) do instrument(ins.n, ins.kind, ins.settings) end
end

-- Plays a MIDI file. Returns the summary (how many tracks and notes, how long).
function M.play(name, opt)
  local song = M.load(name, opt)
  M.instruments(song)
  if opt and opt.first then music(opt.first, table.unpack(song.tracks)) else music(table.unpack(song.tracks)) end
  return song.summary
end

local ORDER = {"duty", "attack", "decay", "sustain", "release", "volume", "pan", "wave"}
local function settings_text(s)
  local t = {}
  for _, k in ipairs(ORDER) do
    if s[k] ~= nil then t[#t + 1] = k .. " = " .. (type(s[k]) == "string" and ("%q"):format(s[k]) or tostring(s[k])) end
  end
  return "{" .. table.concat(t, ", ") .. "}"
end

-- The program that plays the song: its instruments, then music() with its tracks.
function M.text(song, opt)
  local o = {}
  o[#o + 1] = "-- " .. song.summary .. ", made into MML by midi.lua."
  o[#o + 1] = "-- Each track is a voice; " .. (opt and opt.first and "the first is voice " .. opt.first or "they are the last voices") ..
              ". music() stops the music. In a game: dofile(\"this file\") starts it."
  for _, ins in ipairs(song.instruments) do
    o[#o + 1] = ("instrument(%d, %q, %s)   -- %s"):format(ins.n, ins.kind, settings_text(ins.settings), ins.name)
  end
  o[#o + 1] = "music(" .. (opt and opt.first and opt.first .. "," or "")
  for i, t in ipairs(song.tracks) do
    o[#o + 1] = "-- track " .. i .. ": " .. song.parts[i]
    o[#o + 1] = "[=[" .. t .. "]=]" .. (i < #song.tracks and "," or ")")
  end
  return table.concat(o, "\n") .. "\n"
end

-- Writes the program that plays a MIDI file to `out`. Returns the summary.
function M.convert(name, out, opt)
  local song = M.load(name, opt)
  local f, err = io.open(out, "w")
  if not f then fail(tostring(err)) end
  f:write(M.text(song, opt))
  f:close()
  return song.summary
end

return M
