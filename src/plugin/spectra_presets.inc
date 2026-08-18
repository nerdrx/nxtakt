// Spectra v2 factory bank — 49 rows: Init + 48 presets.
// Authored against docs/SPECTRA-PARAMS.md (FROZEN). Categories in contract
// order — Bass 9, Lead 9, Pad 8, Keys 7, Pluck 6, FX 5, Sequence 4 —
// alphabetical within each category. Only non-default overrides are listed;
// loadPreset() resets every id to its default first. Ids ascend within a row.
// This file contains nothing but comments and the SP_PRESET/SP/SP_END macros.

SP_PRESET("Init")
SP_END()

// ---------------------------------------------------------------- Bass (9)

// BA Acid Line — squelchy mono 303-style acid bass: resonant LP24, hot drive,
// ENV2 snap on the cutoff, velocity accents. Macro 1: opens the filter for the sweep.
SP_PRESET("BA Acid Line")
SP(  1, 0.15f)   // A Position
SP( 18, 240.f)   // Cutoff
SP( 19, 0.82f)   // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 8.f)     // Drive
SP( 22, 0.85f)   // Env2>Cutoff
SP( 24, 0.5f)    // Attack
SP( 25, 300.f)   // Decay
SP( 26, 0.55f)   // Sustain
SP( 27, 80.f)    // Release
SP( 29, 180.f)   // E2 Decay
SP( 38, 45.f)    // Glide
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.35f)   // M2 Amt
SP( 98, 1)       // Voice Mode = Mono
SP_END()

// BA Deep Sub — clean legato sub bass: near-sine Fold at low position over a
// big sine sub, gentle LP24. Macro 1: pushes A Position into the wavefold for growl.
SP_PRESET("BA Deep Sub")
SP(  0, 7)       // A Table = Fold
SP(  1, 0.08f)   // A Position
SP( 17, 0.8f)    // Sub
SP( 18, 480.f)   // Cutoff
SP( 20, 3)       // Filter Type = LP24
SP( 21, 3.f)     // Drive
SP( 24, 1.f)     // Attack
SP( 25, 500.f)   // Decay
SP( 26, 0.85f)   // Sustain
SP( 27, 150.f)   // Release
SP( 38, 60.f)    // Glide
SP( 40, 0.8f)    // Master
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 1)       // M1 Dst = A Pos
SP( 70, 0.35f)   // M1 Amt
SP( 98, 2)       // Voice Mode = Legato
SP_END()

// BA Growl FM — through-zero FM growl bass: Harmonic carrier FM'd by a silent
// Bell osc an octave down, LFO2 chews the depth. Macro 1: deepens the FM growl.
SP_PRESET("BA Growl FM")
SP(  0, 2)       // A Table = Harmonic
SP(  1, 0.3f)    // A Position
SP(  8, 4)       // B Table = Bell
SP( 10, -12)     // B Coarse
SP( 18, 900.f)   // Cutoff
SP( 19, 0.3f)    // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 10.f)    // Drive
SP( 22, 0.4f)    // Env2>Cutoff
SP( 26, 0.6f)    // Sustain
SP( 27, 100.f)   // Release
SP( 29, 250.f)   // E2 Decay
SP( 38, 35.f)    // Glide
SP( 48, 6)       // A Warp = FM
SP( 49, 0.45f)   // A Warp Amt
SP( 54, 4.5f)    // L2 Rate
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 3)       // M1 Dst = A Warp Amt
SP( 70, 0.55f)   // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 3)       // M2 Dst = A Warp Amt
SP( 73, 0.2f)    // M2 Amt
SP( 98, 1)       // Voice Mode = Mono
SP_END()

// BA Knuckle — punchy tight mid bass: fast ENV2 knock on an LP24, velocity
// opens the filter per hit. Macro 1: adds drive for a dirtier knuckle.
SP_PRESET("BA Knuckle")
SP(  1, 0.4f)    // A Position
SP( 18, 600.f)   // Cutoff
SP( 19, 0.25f)   // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 6.f)     // Drive
SP( 22, 0.6f)    // Env2>Cutoff
SP( 25, 350.f)   // Decay
SP( 26, 0.45f)   // Sustain
SP( 27, 90.f)    // Release
SP( 29, 120.f)   // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 13)      // M1 Dst = Drive
SP( 70, 0.4f)    // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.3f)    // M2 Amt
SP( 98, 1)       // Voice Mode = Mono
SP_END()

// BA Neon Drop — synthwave legato bass: PWM table with slow LFO width drift,
// small unison, sub underneath, resonant LP24. Macro 1: opens the filter.
SP_PRESET("BA Neon Drop")
SP(  0, 1)       // A Table = PWM
SP(  1, 0.35f)   // A Position
SP(  5, 3)       // A Unison
SP(  6, 12.f)    // A Detune
SP( 17, 0.4f)    // Sub
SP( 18, 320.f)   // Cutoff
SP( 19, 0.62f)   // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 4.f)     // Drive
SP( 32, 0.4f)    // LFO Rate
SP( 34, 0.06f)   // LFO>Position
SP( 38, 55.f)    // Glide
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.6f)    // M1 Amt
SP( 98, 2)       // Voice Mode = Legato
SP_END()

// BA Ring Shift — metallic ring-mod bass: saw ring-modulated by a silent Bell
// osc a fourth up, driven LP24. Macro 1: deepens the ring mod toward full metal.
SP_PRESET("BA Ring Shift")
SP(  1, 0.1f)    // A Position
SP(  8, 4)       // B Table = Bell
SP( 10, 5)       // B Coarse
SP( 18, 700.f)   // Cutoff
SP( 19, 0.3f)    // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 6.f)     // Drive
SP( 26, 0.6f)    // Sustain
SP( 27, 110.f)   // Release
SP( 48, 7)       // A Warp = RM
SP( 49, 0.6f)    // A Warp Amt
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 3)       // M1 Dst = A Warp Amt
SP( 70, 0.4f)    // M1 Amt
SP( 98, 1)       // Voice Mode = Mono
SP_END()

// BA Rubber Glide — elastic mono bass: long portamento on a PWM wave, ENV2
// bounce on the cutoff, LFO wobbles the width. Macro 1: stretches A Position.
SP_PRESET("BA Rubber Glide")
SP(  0, 1)       // A Table = PWM
SP(  1, 0.45f)   // A Position
SP( 18, 500.f)   // Cutoff
SP( 19, 0.5f)    // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 22, 0.5f)    // Env2>Cutoff
SP( 29, 200.f)   // E2 Decay
SP( 32, 5.f)     // LFO Rate
SP( 34, 0.15f)   // LFO>Position
SP( 38, 180.f)   // Glide
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 1)       // M1 Dst = A Pos
SP( 70, 0.45f)   // M1 Amt
SP( 98, 1)       // Voice Mode = Mono
SP_END()

// BA Tidal Wob — 1/8-synced wobble bass: sine LFO rides the LP24 cutoff on a
// Digital wave with sub weight. Macro 1: lifts the wobble's filter centre.
SP_PRESET("BA Tidal Wob")
SP(  0, 5)       // A Table = Digital
SP(  1, 0.5f)    // A Position
SP( 17, 0.5f)    // Sub
SP( 18, 400.f)   // Cutoff
SP( 19, 0.4f)    // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 9.f)     // Drive
SP( 26, 0.8f)    // Sustain
SP( 27, 150.f)   // Release
SP( 33, 6)       // LFO Sync = 1/8
SP( 35, 0.7f)    // LFO>Cutoff
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 98, 1)       // Voice Mode = Mono
SP_END()

// BA Warm Purr — soft polyphonic analog bass: pure saw into a dark LP12 with
// keytracking and a fat sub, slow round envelope. Macro 1: opens the filter.
SP_PRESET("BA Warm Purr")
SP( 17, 0.6f)    // Sub
SP( 18, 350.f)   // Cutoff
SP( 19, 0.15f)   // Resonance
SP( 23, 0.4f)    // Keytrack
SP( 24, 3.f)     // Attack
SP( 25, 600.f)   // Decay
SP( 26, 0.8f)    // Sustain
SP( 27, 200.f)   // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.45f)   // M1 Amt
SP_END()

// ---------------------------------------------------------------- Lead (9)

// LD Anthem Saw — festival supersaw: 7+5 unison voices across both oscs, the
// B layer an octave up, airy noise. Macro 1: raises the octave layer's level.
SP_PRESET("LD Anthem Saw")
SP(  1, 0.05f)   // A Position
SP(  5, 7)       // A Unison
SP(  6, 35.f)    // A Detune
SP(  7, 0.9f)    // A Spread
SP(  8, 0)       // B Table = Basic
SP(  9, 0.05f)   // B Position
SP( 10, 12)      // B Coarse
SP( 12, 0.3f)    // B Level
SP( 13, 5)       // B Unison
SP( 14, 30.f)    // B Detune
SP( 15, 0.8f)    // B Spread
SP( 16, 0.08f)   // Noise
SP( 18, 8000.f)  // Cutoff
SP( 19, 0.15f)   // Resonance
SP( 24, 5.f)     // Attack
SP( 26, 0.8f)    // Sustain
SP( 27, 250.f)   // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 6)       // M1 Dst = B Level
SP( 70, 0.5f)    // M1 Amt
SP_END()

// LD Bend Sync — classic hard-sync sweep lead: ENV3 rakes the sync ratio down
// on every note, mono with a short glide. Macro 1: raises the sync ratio floor.
SP_PRESET("LD Bend Sync")
SP(  1, 0.1f)    // A Position
SP( 18, 9000.f)  // Cutoff
SP( 21, 5.f)     // Drive
SP( 26, 0.75f)   // Sustain
SP( 27, 140.f)   // Release
SP( 38, 30.f)    // Glide
SP( 48, 1)       // A Warp = Sync
SP( 49, 0.25f)   // A Warp Amt
SP( 62, 0.5f)    // E3 Attack
SP( 63, 400.f)   // E3 Decay
SP( 64, 0.2f)    // E3 Sustain
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 3)       // M1 Dst = A Warp Amt
SP( 70, 0.5f)    // M1 Amt
SP( 71, 5)       // M2 Src = ENV3
SP( 72, 3)       // M2 Dst = A Warp Amt
SP( 73, 0.6f)    // M2 Amt
SP( 98, 1)       // Voice Mode = Mono
SP_END()

// LD Chip Quant — 8-bit chiptune lead: full pulse with the read phase
// quantized to a staircase, fast vibrato. Macro 1: crushes the step count further.
SP_PRESET("LD Chip Quant")
SP(  1, 1.f)     // A Position
SP( 18, 12000.f) // Cutoff
SP( 25, 500.f)   // Decay
SP( 27, 60.f)    // Release
SP( 32, 5.5f)    // LFO Rate
SP( 36, 12.f)    // LFO>Pitch
SP( 38, 15.f)    // Glide
SP( 48, 5)       // A Warp = Quantize
SP( 49, 0.55f)   // A Warp Amt
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 3)       // M1 Dst = A Warp Amt
SP( 70, 0.4f)    // M1 Amt
SP( 98, 1)       // Voice Mode = Mono
SP_END()

// LD Fifth Laser — stacked-fifth Digital lead: both oscs on the bright ramp,
// B a fifth up, light unison and drive. Macro 1: sweeps both osc positions.
SP_PRESET("LD Fifth Laser")
SP(  0, 5)       // A Table = Digital
SP(  1, 0.6f)    // A Position
SP(  5, 3)       // A Unison
SP(  6, 20.f)    // A Detune
SP(  8, 5)       // B Table = Digital
SP(  9, 0.6f)    // B Position
SP( 10, 7)       // B Coarse
SP( 12, 0.7f)    // B Level
SP( 13, 3)       // B Unison
SP( 14, 20.f)    // B Detune
SP( 18, 6000.f)  // Cutoff
SP( 19, 0.3f)    // Resonance
SP( 21, 4.f)     // Drive
SP( 27, 200.f)   // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 1)       // M1 Dst = A Pos
SP( 70, 0.4f)    // M1 Amt
SP( 71, 9)       // M2 Src = Macro 1
SP( 72, 2)       // M2 Dst = B Pos
SP( 73, 0.4f)    // M2 Amt
SP_END()

// LD Formant Talk — talking legato lead: Formant table with ENV2 pushing the
// vowel and LFO2 chattering it. Macro 1: sweeps the vowel (A Position).
SP_PRESET("LD Formant Talk")
SP(  0, 3)       // A Table = Formant
SP(  1, 0.2f)    // A Position
SP( 18, 4000.f)  // Cutoff
SP( 19, 0.35f)   // Resonance
SP( 29, 600.f)   // E2 Decay
SP( 30, 0.3f)    // E2 Sustain
SP( 38, 40.f)    // Glide
SP( 41, 0.45f)   // Env2>Position
SP( 54, 0.8f)    // L2 Rate
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 1)       // M1 Dst = A Pos
SP( 70, 0.6f)    // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.15f)   // M2 Amt
SP( 98, 2)       // Voice Mode = Legato
SP_END()

// LD Glass FM — glassy FM keys-lead: Bell carrier FM'd by a silent Harmonic
// osc an octave up, velocity adds bite. Macro 1: raises the FM depth (shine).
SP_PRESET("LD Glass FM")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.25f)   // A Position
SP(  8, 2)       // B Table = Harmonic
SP( 10, 12)      // B Coarse
SP( 18, 10000.f) // Cutoff
SP( 25, 900.f)   // Decay
SP( 26, 0.6f)    // Sustain
SP( 27, 400.f)   // Release
SP( 48, 6)       // A Warp = FM
SP( 49, 0.35f)   // A Warp Amt
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 3)       // M1 Dst = A Warp Amt
SP( 70, 0.4f)    // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 3)       // M2 Dst = A Warp Amt
SP( 73, 0.3f)    // M2 Amt
SP_END()

// LD Pulse Ride — cruising PWM lead: slow LFO sweeps the pulse width, legato
// glide, aftertouch brightens. Macro 1: widens the unison detune fan.
SP_PRESET("LD Pulse Ride")
SP(  0, 1)       // A Table = PWM
SP(  1, 0.5f)    // A Position
SP(  5, 3)       // A Unison
SP(  6, 18.f)    // A Detune
SP( 18, 7500.f)  // Cutoff
SP( 26, 0.8f)    // Sustain
SP( 27, 180.f)   // Release
SP( 32, 0.9f)    // LFO Rate
SP( 34, 0.25f)   // LFO>Position
SP( 38, 25.f)    // Glide
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 14)      // M1 Dst = A Detune
SP( 70, 0.35f)   // M1 Amt
SP( 71, 8)       // M2 Src = Aftertouch
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.25f)   // M2 Amt
SP( 98, 2)       // Voice Mode = Legato
SP_END()

// LD Solo Fold — expressive mono solo voice on the wavefold table: vibrato,
// velocity brightness, aftertouch folds harder. Macro 1: adds drive (scream).
SP_PRESET("LD Solo Fold")
SP(  0, 7)       // A Table = Fold
SP(  1, 0.25f)   // A Position
SP( 18, 6500.f)  // Cutoff
SP( 21, 6.f)     // Drive
SP( 26, 0.8f)    // Sustain
SP( 27, 160.f)   // Release
SP( 32, 5.f)     // LFO Rate
SP( 36, 8.f)     // LFO>Pitch
SP( 38, 50.f)    // Glide
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 13)      // M1 Dst = Drive
SP( 70, 0.5f)    // M1 Amt
SP( 71, 8)       // M2 Src = Aftertouch
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.4f)    // M2 Amt
SP( 74, 6)       // M3 Src = Velocity
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.3f)    // M3 Amt
SP( 98, 1)       // Voice Mode = Mono
SP_END()

// LD Vox Cry — crying vocal lead: Vox table with a slow ENV2 vowel rise, wide
// vibrato and long legato glide. Macro 1: sweeps the vowel (A Position).
SP_PRESET("LD Vox Cry")
SP(  0, 6)       // A Table = Vox
SP(  1, 0.3f)    // A Position
SP( 18, 5000.f)  // Cutoff
SP( 19, 0.25f)   // Resonance
SP( 28, 150.f)   // E2 Attack
SP( 29, 800.f)   // E2 Decay
SP( 32, 5.f)     // LFO Rate
SP( 36, 10.f)    // LFO>Pitch
SP( 38, 70.f)    // Glide
SP( 41, 0.5f)    // Env2>Position
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 1)       // M1 Dst = A Pos
SP( 70, 0.5f)    // M1 Amt
SP( 98, 2)       // Voice Mode = Legato
SP_END()

// ----------------------------------------------------------------- Pad (8)

// PD Aurora Veil — shimmering two-layer pad: wide Harmonic unison under a Vox
// veil, LFO2/3 drift both positions at different rates. Macro 1: opens the filter.
SP_PRESET("PD Aurora Veil")
SP(  0, 2)       // A Table = Harmonic
SP(  1, 0.3f)    // A Position
SP(  5, 5)       // A Unison
SP(  6, 25.f)    // A Detune
SP(  7, 1.f)     // A Spread
SP(  8, 6)       // B Table = Vox
SP(  9, 0.5f)    // B Position
SP( 12, 0.5f)    // B Level
SP( 13, 3)       // B Unison
SP( 18, 3500.f)  // Cutoff
SP( 24, 900.f)   // Attack
SP( 26, 0.85f)   // Sustain
SP( 27, 1800.f)  // Release
SP( 32, 0.11f)   // LFO Rate
SP( 35, 0.15f)   // LFO>Cutoff
SP( 54, 0.07f)   // L2 Rate
SP( 57, 0.05f)   // L3 Rate
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.3f)    // M2 Amt
SP( 74, 3)       // M3 Src = LFO3
SP( 75, 2)       // M3 Dst = B Pos
SP( 76, 0.3f)    // M3 Amt
SP_END()

// PD Breath Choir — airy vocal pad: Vox unison plus a Formant layer with
// keytracked breath noise, slow ENV2 vowel motion. Macro 1: adds breath (noise).
SP_PRESET("PD Breath Choir")
SP(  0, 6)       // A Table = Vox
SP(  1, 0.4f)    // A Position
SP(  5, 4)       // A Unison
SP(  6, 20.f)    // A Detune
SP(  8, 3)       // B Table = Formant
SP(  9, 0.3f)    // B Position
SP( 12, 0.35f)   // B Level
SP( 16, 0.12f)   // Noise
SP( 18, 4200.f)  // Cutoff
SP( 24, 700.f)   // Attack
SP( 26, 0.9f)    // Sustain
SP( 27, 1500.f)  // Release
SP( 28, 900.f)   // E2 Attack
SP( 30, 0.6f)    // E2 Sustain
SP( 41, 0.3f)    // Env2>Position
SP( 44, 0.55f)   // Noise Color
SP( 45, 1)       // Noise Track = On
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 10)      // M1 Dst = Noise Level
SP( 70, 0.5f)    // M1 Amt
SP_END()

// PD Cold Glass — crystalline ring-mod pad: Bell ring-modulated by a silent
// Bell a fifth up, LFO2 drifts the modulator frame. Macro 1: deepens the ring shimmer.
SP_PRESET("PD Cold Glass")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.5f)    // A Position
SP(  8, 4)       // B Table = Bell
SP(  9, 0.3f)    // B Position
SP( 10, 7)       // B Coarse
SP( 18, 6000.f)  // Cutoff
SP( 24, 1200.f)  // Attack
SP( 26, 0.8f)    // Sustain
SP( 27, 2500.f)  // Release
SP( 48, 7)       // A Warp = RM
SP( 49, 0.4f)    // A Warp Amt
SP( 54, 0.06f)   // L2 Rate
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 3)       // M1 Dst = A Warp Amt
SP( 70, 0.4f)    // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 2)       // M2 Dst = B Pos
SP( 73, 0.25f)   // M2 Amt
SP_END()

// PD Dark Matter — sub-heavy dark pad: Harmonic wave an octave down through a
// low LP24, very slow LFO breathes the cutoff. Macro 1: opens the filter.
SP_PRESET("PD Dark Matter")
SP(  0, 2)       // A Table = Harmonic
SP(  1, 0.15f)   // A Position
SP(  2, -12)     // A Coarse
SP(  5, 5)       // A Unison
SP(  6, 30.f)    // A Detune
SP( 17, 0.5f)    // Sub
SP( 18, 800.f)   // Cutoff
SP( 20, 3)       // Filter Type = LP24
SP( 24, 1500.f)  // Attack
SP( 26, 0.9f)    // Sustain
SP( 27, 3000.f)  // Release
SP( 32, 0.06f)   // LFO Rate
SP( 35, 0.2f)    // LFO>Cutoff
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP_END()

// PD Ember Drift — smouldering wavefold pad: Mirror warp folds the Fold table
// while LFO2/3 stir warp depth and position. Macro 1: fans the embers (warp).
SP_PRESET("PD Ember Drift")
SP(  0, 7)       // A Table = Fold
SP(  1, 0.2f)    // A Position
SP(  5, 3)       // A Unison
SP(  6, 22.f)    // A Detune
SP( 18, 2800.f)  // Cutoff
SP( 21, 5.f)     // Drive
SP( 24, 1000.f)  // Attack
SP( 26, 0.85f)   // Sustain
SP( 27, 2200.f)  // Release
SP( 48, 4)       // A Warp = Mirror
SP( 49, 0.3f)    // A Warp Amt
SP( 54, 0.09f)   // L2 Rate
SP( 57, 0.06f)   // L3 Rate
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 3)       // M1 Dst = A Warp Amt
SP( 70, 0.4f)    // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 3)       // M2 Dst = A Warp Amt
SP( 73, 0.25f)   // M2 Amt
SP( 74, 3)       // M3 Src = LFO3
SP( 75, 1)       // M3 Dst = A Pos
SP( 76, 0.2f)    // M3 Amt
SP_END()

// PD Slow Tide — tempo-locked ocean pad: a 4-bar LFO rolls the filter over a
// very wide saw unison, glacial envelope. Macro 1: opens the filter.
SP_PRESET("PD Slow Tide")
SP(  1, 0.3f)    // A Position
SP(  5, 6)       // A Unison
SP(  6, 35.f)    // A Detune
SP(  7, 1.f)     // A Spread
SP( 18, 1200.f)  // Cutoff
SP( 24, 2000.f)  // Attack
SP( 26, 0.9f)    // Sustain
SP( 27, 4000.f)  // Release
SP( 33, 1)       // LFO Sync = 4 bars
SP( 35, 0.35f)   // LFO>Cutoff
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP_END()

// PD Starlight — glittering pad: Bell unison with a Digital sparkle layer an
// octave up, each note panned at random. Macro 1: raises the sparkle layer.
SP_PRESET("PD Starlight")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.35f)   // A Position
SP(  5, 4)       // A Unison
SP(  6, 24.f)    // A Detune
SP(  8, 5)       // B Table = Digital
SP(  9, 0.2f)    // B Position
SP( 10, 12)      // B Coarse
SP( 12, 0.3f)    // B Level
SP( 16, 0.05f)   // Noise
SP( 18, 7000.f)  // Cutoff
SP( 24, 800.f)   // Attack
SP( 26, 0.85f)   // Sustain
SP( 27, 2000.f)  // Release
SP( 57, 0.08f)   // L3 Rate
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 6)       // M1 Dst = B Level
SP( 70, 0.5f)    // M1 Amt
SP( 71, 13)      // M2 Src = Random
SP( 72, 16)      // M2 Dst = Pan
SP( 73, 0.8f)    // M2 Amt
SP( 74, 3)       // M3 Src = LFO3
SP( 75, 2)       // M3 Dst = B Pos
SP( 76, 0.2f)    // M3 Amt
SP_END()

// PD Warm Analog — classic warm string pad: five-voice saw unison into a soft
// keytracked LP12, no tricks, just glow. Macro 1: opens the filter.
SP_PRESET("PD Warm Analog")
SP(  1, 0.12f)   // A Position
SP(  5, 5)       // A Unison
SP(  6, 28.f)    // A Detune
SP(  7, 0.8f)    // A Spread
SP( 17, 0.25f)   // Sub
SP( 18, 1500.f)  // Cutoff
SP( 19, 0.12f)   // Resonance
SP( 23, 0.3f)    // Keytrack
SP( 24, 600.f)   // Attack
SP( 26, 0.9f)    // Sustain
SP( 27, 1200.f)  // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.45f)   // M1 Amt
SP_END()

// ---------------------------------------------------------------- Keys (7)

// KY Bell Piano — hybrid bell piano: Bell strike over a Harmonic octave
// layer, ENV2 softens the attack tone, velocity brightens. Macro 1: brightness.
SP_PRESET("KY Bell Piano")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.3f)    // A Position
SP(  8, 2)       // B Table = Harmonic
SP(  9, 0.2f)    // B Position
SP( 10, 12)      // B Coarse
SP( 12, 0.4f)    // B Level
SP( 18, 2500.f)  // Cutoff
SP( 22, 0.5f)    // Env2>Cutoff
SP( 25, 1500.f)  // Decay
SP( 26, 0.3f)    // Sustain
SP( 27, 600.f)   // Release
SP( 29, 900.f)   // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.35f)   // M2 Amt
SP_END()

// KY Digi Clav — snappy digital clavinet: Digital table with a fast resonant
// ENV2 quack, strong velocity and keytrack. Macro 1: wah (opens the filter).
SP_PRESET("KY Digi Clav")
SP(  0, 5)       // A Table = Digital
SP(  1, 0.35f)   // A Position
SP( 18, 1800.f)  // Cutoff
SP( 19, 0.55f)   // Resonance
SP( 21, 4.f)     // Drive
SP( 22, 0.7f)    // Env2>Cutoff
SP( 23, 0.5f)    // Keytrack
SP( 24, 0.5f)    // Attack
SP( 25, 350.f)   // Decay
SP( 26, 0.25f)   // Sustain
SP( 27, 90.f)    // Release
SP( 29, 140.f)   // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.6f)    // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.4f)    // M2 Amt
SP_END()

// KY Glass Mallet — icy mallet keys: Bell table an octave up with a whisper
// of filtered noise, velocity hardens the strike. Macro 1: sweeps the strike frame.
SP_PRESET("KY Glass Mallet")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.55f)   // A Position
SP(  2, 12)      // A Coarse
SP( 16, 0.06f)   // Noise
SP( 18, 9000.f)  // Cutoff
SP( 25, 900.f)   // Decay
SP( 26, 0.15f)   // Sustain
SP( 27, 500.f)   // Release
SP( 44, 0.8f)    // Noise Color
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 1)       // M1 Dst = A Pos
SP( 70, 0.4f)    // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.3f)    // M2 Amt
SP_END()

// KY Glow EP — DX-style electric piano: light FM on a Bell carrier, velocity
// drives the tine bark, subtle LFO tremolo. Macro 1: tine brightness (FM depth).
SP_PRESET("KY Glow EP")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.15f)   // A Position
SP(  8, 2)       // B Table = Harmonic
SP( 18, 5000.f)  // Cutoff
SP( 25, 1800.f)  // Decay
SP( 26, 0.35f)   // Sustain
SP( 27, 400.f)   // Release
SP( 32, 4.5f)    // LFO Rate
SP( 48, 6)       // A Warp = FM
SP( 49, 0.18f)   // A Warp Amt
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 3)       // M1 Dst = A Warp Amt
SP( 70, 0.3f)    // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 3)       // M2 Dst = A Warp Amt
SP( 73, 0.35f)   // M2 Amt
SP( 74, 1)       // M3 Src = LFO1
SP( 75, 5)       // M3 Dst = A Level
SP( 76, 0.12f)   // M3 Amt
SP_END()

// KY House Organ — percussive drawbar organ: two Harmonic layers an octave
// apart, full sustain, rotary-style LFO. Macro 1: rotary speed (LFO1 rate).
SP_PRESET("KY House Organ")
SP(  0, 2)       // A Table = Harmonic
SP(  1, 0.65f)   // A Position
SP(  8, 2)       // B Table = Harmonic
SP(  9, 0.1f)    // B Position
SP( 10, 12)      // B Coarse
SP( 12, 0.5f)    // B Level
SP( 18, 6000.f)  // Cutoff
SP( 24, 1.f)     // Attack
SP( 25, 200.f)   // Decay
SP( 26, 1.f)     // Sustain
SP( 27, 60.f)    // Release
SP( 32, 0.8f)    // LFO Rate
SP( 35, 0.08f)   // LFO>Cutoff
SP( 36, 5.f)     // LFO>Pitch
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 17)      // M1 Dst = LFO1 Rate
SP( 70, 0.5f)    // M1 Amt
SP_END()

// KY Music Box — delicate music box: Bell table high up the frame, an octave
// raised, zero sustain and strong keytrack. Macro 1: opens the filter (sparkle).
SP_PRESET("KY Music Box")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.7f)    // A Position
SP(  2, 12)      // A Coarse
SP( 18, 7000.f)  // Cutoff
SP( 23, 0.6f)    // Keytrack
SP( 25, 1200.f)  // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 800.f)   // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.35f)   // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.3f)    // M2 Amt
SP_END()

// KY Velvet Wurli — smoky reed piano: dark Harmonic frame with drive bark on
// velocity and a slow amp tremolo. Macro 1: adds bark (drive).
SP_PRESET("KY Velvet Wurli")
SP(  0, 2)       // A Table = Harmonic
SP(  1, 0.05f)   // A Position
SP( 18, 3200.f)  // Cutoff
SP( 21, 5.f)     // Drive
SP( 25, 1400.f)  // Decay
SP( 26, 0.4f)    // Sustain
SP( 27, 300.f)   // Release
SP( 32, 5.f)     // LFO Rate
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 13)      // M1 Dst = Drive
SP( 70, 0.4f)    // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 13)      // M2 Dst = Drive
SP( 73, 0.4f)    // M2 Amt
SP( 74, 1)       // M3 Src = LFO1
SP( 75, 5)       // M3 Dst = A Level
SP( 76, 0.15f)   // M3 Amt
SP_END()

// --------------------------------------------------------------- Pluck (6)

// PL Bright Karp — plucked-string model: pulse plus keytracked bright noise
// burst, fast ENV2 damping like a real string. Macro 1: opens the filter.
SP_PRESET("PL Bright Karp")
SP(  1, 0.85f)   // A Position
SP( 16, 0.25f)   // Noise
SP( 18, 900.f)   // Cutoff
SP( 22, 0.75f)   // Env2>Cutoff
SP( 23, 0.6f)    // Keytrack
SP( 25, 450.f)   // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 450.f)   // Release
SP( 29, 200.f)   // E2 Decay
SP( 44, 0.9f)    // Noise Color
SP( 45, 1)       // Noise Track = On
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.3f)    // M2 Amt
SP_END()

// PL Dew Drop — liquid bell pluck: ENV2 pulls the Bell frame down as each
// note fades, notes scatter across the stereo field. Macro 1: raises the frame.
SP_PRESET("PL Dew Drop")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.4f)    // A Position
SP( 18, 8000.f)  // Cutoff
SP( 25, 600.f)   // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 900.f)   // Release
SP( 41, -0.5f)   // Env2>Position
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 1)       // M1 Dst = A Pos
SP( 70, 0.5f)    // M1 Amt
SP( 71, 13)      // M2 Src = Random
SP( 72, 16)      // M2 Dst = Pan
SP( 73, 0.6f)    // M2 Amt
SP_END()

// PL Fold Harp — golden harp pluck: ENV3 flicks the Fold frame open at the
// attack then lets it settle, keytracked. Macro 1: raises the fold base frame.
SP_PRESET("PL Fold Harp")
SP(  0, 7)       // A Table = Fold
SP(  1, 0.15f)   // A Position
SP( 18, 11000.f) // Cutoff
SP( 23, 0.5f)    // Keytrack
SP( 25, 700.f)   // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 1000.f)  // Release
SP( 63, 350.f)   // E3 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 1)       // M1 Dst = A Pos
SP( 70, 0.4f)    // M1 Amt
SP( 71, 5)       // M2 Src = ENV3
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.5f)    // M2 Amt
SP_END()

// PL Ice Pizz — tight icy pizzicato: Digital table, very short resonant ENV2
// blip, slight unison shimmer. Macro 1: sharpens the resonance.
SP_PRESET("PL Ice Pizz")
SP(  0, 5)       // A Table = Digital
SP(  1, 0.3f)    // A Position
SP(  5, 2)       // A Unison
SP(  6, 10.f)    // A Detune
SP( 18, 700.f)   // Cutoff
SP( 19, 0.4f)    // Resonance
SP( 22, 0.8f)    // Env2>Cutoff
SP( 25, 220.f)   // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 250.f)   // Release
SP( 29, 90.f)    // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 12)      // M1 Dst = Resonance
SP( 70, 0.5f)    // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.35f)   // M2 Amt
SP_END()

// PL Rubber Band — twangy snap pluck: ENV3 snaps a Bend+ phase warp at the
// attack for an elastic thwack. Macro 1: raises the bend depth (twang).
SP_PRESET("PL Rubber Band")
SP(  1, 0.5f)    // A Position
SP( 18, 3000.f)  // Cutoff
SP( 26, 0.f)     // Sustain
SP( 27, 350.f)   // Release
SP( 48, 2)       // A Warp = Bend+
SP( 49, 0.15f)   // A Warp Amt
SP( 63, 180.f)   // E3 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 3)       // M1 Dst = A Warp Amt
SP( 70, 0.4f)    // M1 Amt
SP( 71, 5)       // M2 Src = ENV3
SP( 72, 3)       // M2 Dst = A Warp Amt
SP( 73, 0.6f)    // M2 Amt
SP_END()

// PL Vox Dab — vocal stab pluck: Vox frame kicked by ENV2, every note lands
// on a slightly different vowel via Random. Macro 1: opens the filter.
SP_PRESET("PL Vox Dab")
SP(  0, 6)       // A Table = Vox
SP(  1, 0.55f)   // A Position
SP( 18, 4500.f)  // Cutoff
SP( 25, 380.f)   // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 420.f)   // Release
SP( 29, 160.f)   // E2 Decay
SP( 41, 0.6f)    // Env2>Position
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 13)      // M2 Src = Random
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.2f)    // M2 Amt
SP_END()

// ------------------------------------------------------------------ FX (5)

// FX Cave Drone — cavernous drone: low Formant unison and sub, three slow
// free LFOs stir position, cutoff and resonance. Macro 1: opens the cave mouth.
SP_PRESET("FX Cave Drone")
SP(  0, 3)       // A Table = Formant
SP(  1, 0.1f)    // A Position
SP(  2, -12)     // A Coarse
SP(  5, 5)       // A Unison
SP(  6, 45.f)    // A Detune
SP(  7, 1.f)     // A Spread
SP( 17, 0.4f)    // Sub
SP( 18, 900.f)   // Cutoff
SP( 19, 0.5f)    // Resonance
SP( 24, 3000.f)  // Attack
SP( 26, 1.f)     // Sustain
SP( 27, 6000.f)  // Release
SP( 32, 0.08f)   // LFO Rate
SP( 54, 0.03f)   // L2 Rate
SP( 57, 0.05f)   // L3 Rate
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.4f)    // M2 Amt
SP( 74, 3)       // M3 Src = LFO3
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.25f)   // M3 Amt
SP( 77, 1)       // M4 Src = LFO1
SP( 78, 12)      // M4 Dst = Resonance
SP( 79, 0.3f)    // M4 Amt
SP_END()

// FX Glitch Field — broken-machine texture: Random re-pitches, re-pans and
// re-frames every note while an S&H LFO chops the filter. Macro 1: glitch speed.
SP_PRESET("FX Glitch Field")
SP(  0, 5)       // A Table = Digital
SP(  1, 0.5f)    // A Position
SP( 16, 0.2f)    // Noise
SP( 18, 4000.f)  // Cutoff
SP( 25, 200.f)   // Decay
SP( 26, 0.6f)    // Sustain
SP( 32, 9.f)     // LFO Rate
SP( 35, 0.6f)    // LFO>Cutoff
SP( 37, 4)       // LFO Shape = S&H
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 17)      // M1 Dst = LFO1 Rate
SP( 70, 0.6f)    // M1 Amt
SP( 71, 13)      // M2 Src = Random
SP( 72, 7)       // M2 Dst = A Pitch
SP( 73, 0.5f)    // M2 Amt
SP( 74, 13)      // M3 Src = Random
SP( 75, 16)      // M3 Dst = Pan
SP( 76, 1.f)     // M3 Amt
SP( 77, 13)      // M4 Src = Random
SP( 78, 1)       // M4 Dst = A Pos
SP( 79, 0.6f)    // M4 Amt
SP_END()

// FX Metal Rain — inharmonic ring-mod clangs: Bell rung by a detuned silent
// Bell, every hit retuned by Random. Macro 1: retunes the ring modulator upward.
SP_PRESET("FX Metal Rain")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.6f)    // A Position
SP(  8, 4)       // B Table = Bell
SP( 10, 13)      // B Coarse
SP( 11, 30.f)    // B Fine
SP( 16, 0.15f)   // Noise
SP( 18, 7000.f)  // Cutoff
SP( 22, -0.4f)   // Env2>Cutoff
SP( 25, 900.f)   // Decay
SP( 26, 0.2f)    // Sustain
SP( 27, 700.f)   // Release
SP( 29, 400.f)   // E2 Decay
SP( 44, 0.7f)    // Noise Color
SP( 48, 7)       // A Warp = RM
SP( 49, 0.85f)   // A Warp Amt
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 8)       // M1 Dst = B Pitch
SP( 70, 0.3f)    // M1 Amt
SP( 71, 13)      // M2 Src = Random
SP( 72, 8)       // M2 Dst = B Pitch
SP( 73, 0.4f)    // M2 Amt
SP_END()

// FX Solar Wind — resonant wind bed: colored noise through a slowly howling
// bandpass-like resonant sweep, faint Vox tone beneath. Macro 1: storm swell.
SP_PRESET("FX Solar Wind")
SP(  0, 6)       // A Table = Vox
SP(  1, 0.5f)    // A Position
SP(  4, 0.15f)   // A Level
SP( 16, 0.9f)    // Noise
SP( 18, 1500.f)  // Cutoff
SP( 19, 0.65f)   // Resonance
SP( 24, 1200.f)  // Attack
SP( 26, 1.f)     // Sustain
SP( 27, 3000.f)  // Release
SP( 32, 0.07f)   // LFO Rate
SP( 35, 0.45f)   // LFO>Cutoff
SP( 44, 0.35f)   // Noise Color
SP( 54, 0.2f)    // L2 Rate
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.6f)    // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 12)      // M2 Dst = Resonance
SP( 73, 0.2f)    // M2 Amt
SP_END()

// FX Void Drone — abyssal drone: mirrored Fold two octaves down over a -2 oct
// sub, three glacial LFOs bend warp and positions. Macro 1: lets light in (cutoff).
SP_PRESET("FX Void Drone")
SP(  0, 7)       // A Table = Fold
SP(  1, 0.3f)    // A Position
SP(  2, -24)     // A Coarse
SP(  8, 3)       // B Table = Formant
SP(  9, 0.7f)    // B Position
SP( 10, -12)     // B Coarse
SP( 12, 0.4f)    // B Level
SP( 17, 0.6f)    // Sub
SP( 18, 600.f)   // Cutoff
SP( 24, 4000.f)  // Attack
SP( 26, 1.f)     // Sustain
SP( 27, 8000.f)  // Release
SP( 32, 0.06f)   // LFO Rate
SP( 34, 0.3f)    // LFO>Position
SP( 43, -2)      // Sub Oct
SP( 48, 4)       // A Warp = Mirror
SP( 49, 0.5f)    // A Warp Amt
SP( 54, 0.02f)   // L2 Rate
SP( 57, 0.04f)   // L3 Rate
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.45f)   // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 3)       // M2 Dst = A Warp Amt
SP( 73, 0.3f)    // M2 Amt
SP( 74, 3)       // M3 Src = LFO3
SP( 75, 2)       // M3 Dst = B Pos
SP( 76, 0.5f)    // M3 Amt
SP_END()

// ------------------------------------------------------------ Sequence (4)

// SQ Acid Step — 1/16-gated acid sequence: square LFO1 gates the level, LFO2
// sweeps the filter over each bar, mono glide ties steps. Macro 1: filter lift.
SP_PRESET("SQ Acid Step")
SP(  1, 0.15f)   // A Position
SP(  4, 0.5f)    // A Level
SP( 18, 300.f)   // Cutoff
SP( 19, 0.7f)    // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 7.f)     // Drive
SP( 22, 0.7f)    // Env2>Cutoff
SP( 26, 0.6f)    // Sustain
SP( 27, 100.f)   // Release
SP( 29, 150.f)   // E2 Decay
SP( 33, 7)       // LFO Sync = 1/16
SP( 37, 3)       // LFO Shape = Square
SP( 38, 40.f)    // Glide
SP( 55, 3)       // L2 Sync = 1 bar
SP( 56, 2)       // L2 Shape = Saw
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 5)       // M2 Dst = A Level
SP( 73, 0.5f)    // M2 Amt
SP( 74, 2)       // M3 Src = LFO2
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.3f)    // M3 Amt
SP( 98, 1)       // Voice Mode = Mono
SP_END()

// SQ Chip Random — chiptune roulette: an S&H LFO re-pitches held notes every
// 1/16 while a square LFO2 gates the level. Macro 1: opens the filter.
SP_PRESET("SQ Chip Random")
SP(  0, 5)       // A Table = Digital
SP(  1, 0.8f)    // A Position
SP(  4, 0.5f)    // A Level
SP( 18, 9000.f)  // Cutoff
SP( 26, 0.8f)    // Sustain
SP( 27, 80.f)    // Release
SP( 33, 7)       // LFO Sync = 1/16
SP( 37, 4)       // LFO Shape = S&H
SP( 55, 7)       // L2 Sync = 1/16
SP( 56, 3)       // L2 Shape = Square
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 7)       // M2 Dst = A Pitch
SP( 73, 0.5f)    // M2 Amt
SP( 74, 1)       // M3 Src = LFO1
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.4f)    // M3 Amt
SP( 77, 2)       // M4 Src = LFO2
SP( 78, 5)       // M4 Dst = A Level
SP( 79, 0.5f)    // M4 Amt
SP_END()

// SQ Pump Gate — sidechain-pump chords: a 1/4-synced saw LFO ducks the level
// like a kick sidechain, a 4-bar LFO2 drifts the frame. Macro 1: opens the filter.
SP_PRESET("SQ Pump Gate")
SP(  1, 0.2f)    // A Position
SP(  5, 5)       // A Unison
SP(  6, 30.f)    // A Detune
SP(  7, 0.9f)    // A Spread
SP( 18, 2500.f)  // Cutoff
SP( 24, 10.f)    // Attack
SP( 26, 1.f)     // Sustain
SP( 27, 400.f)   // Release
SP( 33, 5)       // LFO Sync = 1/4
SP( 37, 2)       // LFO Shape = Saw
SP( 55, 1)       // L2 Sync = 4 bars
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 5)       // M2 Dst = A Level
SP( 73, -0.5f)   // M2 Amt
SP( 74, 2)       // M3 Src = LFO2
SP( 75, 1)       // M3 Dst = A Pos
SP( 76, 0.3f)    // M3 Amt
SP_END()

// SQ Trance Gate — polyrhythmic trance gate: A gated at 1/16, the PWM B layer
// gated at 1/8, a 1-bar LFO3 rolls the filter. Macro 1: opens the filter.
SP_PRESET("SQ Trance Gate")
SP(  1, 0.1f)    // A Position
SP(  4, 0.45f)   // A Level
SP(  5, 5)       // A Unison
SP(  6, 25.f)    // A Detune
SP(  7, 1.f)     // A Spread
SP(  9, 0.4f)    // B Position
SP( 12, 0.45f)   // B Level
SP( 18, 1800.f)  // Cutoff
SP( 19, 0.3f)    // Resonance
SP( 26, 1.f)     // Sustain
SP( 27, 300.f)   // Release
SP( 33, 7)       // LFO Sync = 1/16
SP( 37, 3)       // LFO Shape = Square
SP( 55, 6)       // L2 Sync = 1/8
SP( 56, 3)       // L2 Shape = Square
SP( 58, 3)       // L3 Sync = 1 bar
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 5)       // M2 Dst = A Level
SP( 73, 0.6f)    // M2 Amt
SP( 74, 2)       // M3 Src = LFO2
SP( 75, 6)       // M3 Dst = B Level
SP( 76, 0.5f)    // M3 Amt
SP( 77, 3)       // M4 Src = LFO3
SP( 78, 11)      // M4 Dst = Cutoff
SP( 79, 0.3f)    // M4 Amt
SP_END()
