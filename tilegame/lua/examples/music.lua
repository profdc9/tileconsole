-- Music: eight bars (C, Am, F, G, twice) on four tracks, a pulse lead, a softer pulse under it, a triangle bass and noise drums, looping.
-- It plays on at the prompt after the program ends: music() stops it, musicvolume(n) makes it softer (0 to 255).
-- The four tracks are on voices 13 to 16, so a game's sound effects can use 1 to 12.

instrument(1, "pulse", {duty = 64, decay = 300, sustain = 120, release = 80, volume = 150})   -- the lead: a thin pulse that fades a little
instrument(2, "pulse", {decay = 200, sustain = 150, release = 100, volume = 70})              -- under it: a square wave, quiet
instrument(3, "triangle", {decay = 150, sustain = 200, release = 40, volume = 230})            -- the bass
instrument(4, "noise", {decay = 80, sustain = 0, volume = 220})                               -- kick: a low noise, short
instrument(5, "noise", {decay = 150, sustain = 0, volume = 150})                              -- snare: a bright noise
instrument(6, "buzz", {decay = 30, sustain = 0, volume = 70})                                 -- hi-hat: the metallic noise, very short

local lead = [[
  t140 L @1 o5 l8
  e g >c< g e4 g4  | a >c e c< a4 >c4< | f a >c< a f4 a4 | g b >d< b g2 |
  >c4< g4 e4 g4    | a4 >c4 e4 c4<     | a4 f4 g4 a4     | g2 r2        ]]

local harmony = [[
  L @2 o4 l2
  [ e g | e a | f a | d g ]2 ]]

local bass = [[
  L @3 l4
  [ o3 c o2 g o3 c o2 g | o2 a o3 e o2 a o3 e | o2 f o3 c o2 f o3 c | o2 g o3 d o2 g o3 d ]2 ]]

local drums = [[
  L l8
  [ @4 n60 @6 n127 @5 n120 @6 n127 ]16 ]]

music(lead, harmony, bass, drums)
print("The music plays on: music() stops it, musicvolume(n) sets its volume.")
