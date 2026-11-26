-- Sounds: each button plays one, and its square lights up while it sounds (playing).
--   A      a jump: a pulse that slides up
--   X      a coin: two quick notes
--   Y      an explosion: noise that falls
--   arrows the notes of a chord on a wavetable voice, each for 150 ms
--   Start  a drum, made with sample() from a table of numbers
--   B      ends the program
-- On the keyboard: Z X A S are A B X Y, Return is Start.

gfx(8)
palette("1", 50, 50, 70)        -- a square while its voice is quiet
palette("2", 255, 210, 60)      -- ... and while it plays
tiledef(1, string.rep("1", 64))
tiledef(2, string.rep("2", 64))
newmap(40, 30)

JUMP, COIN, BOOM, NOTE, DRUM = 1, 2, 3, 4, 5   -- a voice for each kind of sound

envelope(JUMP, 0, 250, 0, 20)   -- no attack, gone in 250 ms: sustain 0 ends the note by itself
duty(JUMP, 64)                  -- a thin pulse (128 is a square wave)
volume(JUMP, 150)

envelope(COIN, 0, 350, 0, 50)
volume(COIN, 120)

envelope(BOOM, 0, 700, 0, 50)
volume(BOOM, 220)

wave(NOTE, "triangle")
envelope(NOTE, 5, 150, 140, 250)   -- held at 140 until the note's 150 ms are up, then 250 ms to fade
volume(NOTE, 170)

local d = {}                       -- the drum: 100 ms of noise that dies away, at 24000 samples a second
for i = 1, 2400 do d[i] = (math.random() * 2 - 1) * (1 - i / 2400) ^ 3 end
sample(1, d, 24000)

local squares = {JUMP, COIN, BOOM, NOTE, DRUM}
local chord = {up = 72, down = 60, left = 64, right = 67}   -- C E G C (60 is middle C)
local coin_at

function update(k)
  if pressed(0, "a") then
    tone(JUMP, "pulse", 60)
    slide(JUMP, 84, 200)             -- two octaves up in 200 ms
  end
  if pressed(0, "x") then
    tone(COIN, "pulse", "B5")
    coin_at = millis()
  end
  if coin_at and millis() - coin_at >= 70 then   -- the coin's second note
    pitch(COIN, "E6")
    coin_at = nil
  end
  if pressed(0, "y") then
    tone(BOOM, "noise", 110)         -- the noise's clock, a high one: a hiss ...
    slide(BOOM, 70, 700)             -- ... falling to a rumble
  end
  for name, n in pairs(chord) do
    if pressed(0, name) then tone(NOTE, "wave", n, 150) end
  end
  if pressed(0, "start") then play(DRUM, 1) end
  for i, v in ipairs(squares) do
    tilefill(i * 6, 13, 4, 4, playing(v) and 2 or 1)
  end
  return btn(0, "b")
end

onframe(update)
run()
silence()
