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

// ===========================================================================
// Spectra v3 factory bank 2 — 48 more presets, ids 49..96 of the bank.
//
// Bank 1 above is frozen (a preset index is not stored in any set, but the
// user-preset contract freezes the factory ORDER, so bank 2 appends rather
// than interleaves). Same seven categories in contract order — Bass 9, Lead
// 9, Pad 8, Keys 7, Pluck 6, FX 5, Sequence 4 — alphabetical within bank 2's
// own group of each. The editor draws a header wherever the tag changes, so
// the popover shows the seven categories twice: bank 1's, then bank 2's.
//
// Where bank 1 was built on warp, unison and the matrix, this one is built on
// what v3 added: drawn 16-step LFO grids (SPLFO), one-shot LFOs used as extra
// envelopes (L1/L2/L3 Mode), the MIDI sources (mod wheel, pitch bend, one
// learned CC via SPCC), per-slot response curves (ids 101..108), and four
// imported factory wavetables (SPWTA/SPWTB, slot 8).
//
// Two authoring facts worth stating once, because many rows below rely on
// them:
//   * A Custom-shape LFO is UNIPOLAR 0..1 and its step level is d/15 for the
//     hex digit d. A matrix slot into A Pitch is ±24 st at full scale, so an
//     Amt of exactly 0.625 makes ONE HEX DIGIT ONE SEMITONE (24 · 0.625 = 15,
//     and 15 · d/15 = d). Every pitch-staircase row below uses 0.625f, and
//     its grid can be read as a note list: "0479" is a major sixth chord.
//   * A one-shot LFO is bipolar for shapes 0..4 and holds its value at phase
//     1.0 — saw holds +1, triangle and square hold −1, sine returns to 0. A
//     saw one-shot into A Pitch therefore starts at −Amt and ENDS at +Amt; the
//     drop patches offset that with A Coarse so the fall lands exactly on the
//     played note (and the sub, which does not follow Coarse or the matrix,
//     holds the root underneath the whole drop).
//
// The four imported tables (7 frames × 2048, ≤ 64 KiB each), by hash:
//   16b407f0a07b9117  reso sweep      — a resonant lowpass sweep frozen into
//                                       frames; the peak climbs H2 → H24.
//   5c0d37c1e4244576  formant vowels  — three formants walking u o a e i.
//   33f9d0639a00d4f0  harmonic drift  — odd-only (hollow) filling to full,
//                                       every partial's phase rotating.
//   c452993b482418f3  metal comb      — fourteen irregular partials under a
//                                       walking comb; gong to bar chime.
// ===========================================================================

// --------------------------------------------------------- Bass (9), bank 2

// BA Bit Ladder — a stepped filter ladder: a drawn 16-step grid walks the LP24
// cutoff in 1/16s under a mono acid bass, velocity biting the drive.
// Macro 1: raises the ladder's floor (cutoff).
SP_PRESET("BA Bit Ladder")
SP(  1, 0.2f)    // A Position
SP( 17, 0.35f)   // Sub
SP( 18, 180.f)   // Cutoff
SP( 19, 0.7f)    // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 7.f)     // Drive
SP( 26, 0.75f)   // Sustain
SP( 27, 90.f)    // Release
SP( 33, 3)       // LFO Sync = 1 bar (16 steps over four beats = 1/16 steps)
SP( 37, 5)       // LFO Shape = Custom
SP( 38, 25.f)    // Glide
SP( 40, 0.4f)    // Master
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.45f)   // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.55f)   // M2 Amt
SP( 74, 6)       // M3 Src = Velocity
SP( 75, 13)      // M3 Dst = Drive
SP( 76, 0.3f)    // M3 Amt
SP( 98, 1)       // Voice Mode = Mono
SPLFO(1, "f8c4a2f6c840a284", 0.06f)
SP_END()

// BA Drop Kick — the 808 drop as a modulation: a one-shot saw LFO2 falls two
// octaves in a third of a second onto the played note, over a fat sine sub.
// Macro 1: weight (Sub Level).
SP_PRESET("BA Drop Kick")
SP(  1, 0.05f)   // A Position
SP(  2, 12)      // A Coarse — centres the bipolar one-shot so the fall lands on the note
SP( 17, 0.9f)    // Sub
SP( 18, 700.f)   // Cutoff
SP( 20, 3)       // Filter Type = LP24
SP( 21, 5.f)     // Drive
SP( 24, 0.5f)    // Attack
SP( 25, 900.f)   // Decay
SP( 26, 0.35f)   // Sustain
SP( 27, 200.f)   // Release
SP( 54, 3.f)     // L2 Rate — one shot completes in 333 ms
SP( 56, 2)       // L2 Shape = Saw
SP( 60, 1)       // L2 Mode = One-shot
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 9)       // M1 Dst = Sub Level
SP( 70, 0.4f)    // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 7)       // M2 Dst = A Pitch
SP( 73, -0.5f)   // M2 Amt — +12 st at note-on falling to -12 st, i.e. 24 down to 0
SP( 98, 1)       // Voice Mode = Mono
SP_END()

// BA Gate Grid — a sequenced bass with no sequencer: one drawn grid IS the
// level (base 0, so the grid gates outright) while a slower second grid rolls
// the filter across two bars. Macro 1: opens the filter.
SP_PRESET("BA Gate Grid")
SP(  1, 0.3f)    // A Position
SP(  4, 0.f)     // A Level — the grid is the level
SP( 18, 900.f)   // Cutoff
SP( 19, 0.35f)   // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 6.f)     // Drive
SP( 26, 1.f)     // Sustain
SP( 27, 60.f)    // Release
SP( 33, 3)       // LFO Sync = 1 bar
SP( 37, 5)       // LFO Shape = Custom
SP( 55, 2)       // L2 Sync = 2 bars
SP( 56, 5)       // L2 Shape = Custom
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 5)       // M2 Dst = A Level
SP( 73, 1.f)     // M2 Amt
SP( 74, 2)       // M3 Src = LFO2
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.4f)    // M3 Amt
SP( 98, 1)       // Voice Mode = Mono
SPLFO(1, "fcf8f4c8f0c4f860", 0.02f)
SPLFO(2, "8ac6f8a4c6f8a642", 0.35f)
SP_END()

// BA Reese Curve — the curve is the patch: Macro 1 opens the unison detune
// through an Exp response, so the first half of the knob is fine tuning and
// the last quarter is the full reese. Macro 1: widens the beat (A Detune).
SP_PRESET("BA Reese Curve")
SP(  5, 3)       // A Unison
SP(  6, 8.f)     // A Detune
SP(  7, 0.35f)   // A Spread
SP( 17, 0.3f)    // Sub
SP( 18, 420.f)   // Cutoff
SP( 19, 0.2f)    // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 4.f)     // Drive
SP( 26, 0.85f)   // Sustain
SP( 27, 180.f)   // Release
SP( 38, 40.f)    // Glide
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 14)      // M1 Dst = A Detune
SP( 70, 0.75f)   // M1 Amt
SP( 71, 9)       // M2 Src = Macro 1
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.25f)   // M2 Amt
SP( 98, 1)       // Voice Mode = Mono
SP(101, 1)       // M1 Curve = Exp
SP(102, 2)       // M2 Curve = S-curve
SP_END()

// BA Slap Thumb — a one-shot square LFO3 is the thumb: 42 ms of bright bark at
// note-on, then it flips negative and stays there, damping the tail.
// Macro 1: adds grit (Drive).
SP_PRESET("BA Slap Thumb")
SP(  1, 0.55f)   // A Position
SP( 18, 800.f)   // Cutoff
SP( 19, 0.3f)    // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 3.f)     // Drive
SP( 23, 0.35f)   // Keytrack
SP( 25, 250.f)   // Decay
SP( 26, 0.5f)    // Sustain
SP( 27, 80.f)    // Release
SP( 57, 12.f)    // L3 Rate — the half cycle that barks is 42 ms
SP( 59, 3)       // L3 Shape = Square
SP( 61, 1)       // L3 Mode = One-shot
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 13)      // M1 Dst = Drive
SP( 70, 0.5f)    // M1 Amt
SP( 71, 3)       // M2 Src = LFO3
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.45f)   // M2 Amt
SP( 74, 6)       // M3 Src = Velocity
SP( 75, 13)      // M3 Dst = Drive
SP( 76, 0.35f)   // M3 Amt
SP( 98, 1)       // Voice Mode = Mono
SP_END()

// BA Sweep Table — the imported "reso sweep" table is the filter: ENV2 pulls
// the frozen resonant peak down through every note while the SVF just keeps
// the top polite. Macro 1: rides the sweep (A Position).
SP_PRESET("BA Sweep Table")
SP(  0, 8)       // A Table = custom slot 8
SP(  1, 0.15f)   // A Position
SP( 17, 0.45f)   // Sub
SP( 18, 3000.f)  // Cutoff
SP( 20, 3)       // Filter Type = LP24
SP( 21, 5.f)     // Drive
SP( 26, 0.6f)    // Sustain
SP( 27, 140.f)   // Release
SP( 29, 220.f)   // E2 Decay
SP( 38, 30.f)    // Glide
SP( 41, 0.45f)   // Env2>Position
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 1)       // M1 Dst = A Pos
SP( 70, 0.55f)   // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.25f)   // M2 Amt
SP( 98, 1)       // Voice Mode = Mono
SPWTA("16b407f0a07b9117")
SP_END()

// BA Talk Box — the learned CC (11, expression) is the mouth: the pedal walks
// the Formant vowel and opens the throat behind it, bend left at the vocal
// default of 2 st. Macro 1: opens the throat (cutoff).
SP_PRESET("BA Talk Box")
SP(  0, 3)       // A Table = Formant
SP(  1, 0.2f)    // A Position
SP( 17, 0.4f)    // Sub
SP( 18, 2200.f)  // Cutoff
SP( 19, 0.4f)    // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 6.f)     // Drive
SP( 26, 0.8f)    // Sustain
SP( 27, 130.f)   // Release
SP( 38, 45.f)    // Glide
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 16)      // M2 Src = MIDI CC
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.8f)    // M2 Amt
SP( 74, 16)      // M3 Src = MIDI CC
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.3f)    // M3 Amt
SP( 98, 1)       // Voice Mode = Mono
SP(103, 2)       // M3 Curve = S-curve
SPCC(11)
SP_END()

// BA Voltage Step — a modular pitch sequencer in one grid: 1/8 steps, one hex
// digit per semitone, root-heavy with octave and fifth jumps, hard steps and
// no glide. Macro 1: opens the filter.
SP_PRESET("BA Voltage Step")
SP(  1, 0.25f)   // A Position
SP( 18, 1400.f)  // Cutoff
SP( 19, 0.45f)   // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 6.f)     // Drive
SP( 22, 0.4f)    // Env2>Cutoff
SP( 25, 260.f)   // Decay
SP( 26, 0.4f)    // Sustain
SP( 27, 70.f)    // Release
SP( 29, 130.f)   // E2 Decay
SP( 55, 2)       // L2 Sync = 2 bars (16 steps over eight beats = 1/8 steps)
SP( 56, 5)       // L2 Shape = Custom
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 7)       // M2 Dst = A Pitch
SP( 73, 0.625f)  // M2 Amt — one hex digit = one semitone
SP( 98, 1)       // Voice Mode = Mono
SPLFO(2, "00c00c700ca0c730", 0.f)
SP_END()

// BA Wheel Growl — the mod wheel is the transformation: at rest a clean saw,
// at full travel a through-zero FM growl an octave down, arriving late through
// an Exp curve. Macro 1: adds drive.
SP_PRESET("BA Wheel Growl")
SP(  1, 0.2f)    // A Position
SP(  8, 4)       // B Table = Bell
SP( 10, -12)     // B Coarse
SP( 18, 900.f)   // Cutoff
SP( 19, 0.3f)    // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 4.f)     // Drive
SP( 26, 0.8f)    // Sustain
SP( 27, 110.f)   // Release
SP( 38, 35.f)    // Glide
SP( 48, 6)       // A Warp = FM
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 13)      // M1 Dst = Drive
SP( 70, 0.45f)   // M1 Amt
SP( 71, 14)      // M2 Src = Mod Wheel
SP( 72, 3)       // M2 Dst = A Warp Amt
SP( 73, 0.7f)    // M2 Amt
SP( 74, 14)      // M3 Src = Mod Wheel
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.3f)    // M3 Amt
SP( 98, 1)       // Voice Mode = Mono
SP(102, 1)       // M2 Curve = Exp
SP_END()

// --------------------------------------------------------- Lead (9), bank 2

// LD Bend Whammy — a whammy-bar lead: the pitch wheel is a full octave and
// also opens the filter through an S-curve, so a dive darkens and a pull
// screams. Macro 1: scream (Drive).
SP_PRESET("LD Bend Whammy")
SP(  1, 0.1f)    // A Position
SP(  5, 3)       // A Unison
SP(  6, 20.f)    // A Detune
SP( 18, 7000.f)  // Cutoff
SP( 19, 0.25f)   // Resonance
SP( 21, 5.f)     // Drive
SP( 26, 0.8f)    // Sustain
SP( 27, 160.f)   // Release
SP( 38, 20.f)    // Glide
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 13)      // M1 Dst = Drive
SP( 70, 0.5f)    // M1 Amt
SP( 71, 15)      // M2 Src = Pitch Bend
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.35f)   // M2 Amt
SP( 74, 6)       // M3 Src = Velocity
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.3f)    // M3 Amt
SP( 98, 1)       // Voice Mode = Mono
SP( 99, 12)      // Bend Range = 12 st
SP(102, 2)       // M2 Curve = S-curve
SP_END()

// LD Chase Steps — a drawn arpeggio the keyboard transposes: 1/16 steps, one
// digit per semitone, reading 0-4-7-9 / 0-4-7-12 / 0-4-7-9 / 0-4-7-14.
// Macro 1: opens the filter.
SP_PRESET("LD Chase Steps")
SP(  0, 5)       // A Table = Digital
SP(  1, 0.55f)   // A Position
SP( 18, 9000.f)  // Cutoff
SP( 19, 0.2f)    // Resonance
SP( 25, 700.f)   // Decay
SP( 26, 0.75f)   // Sustain
SP( 27, 90.f)    // Release
SP( 33, 3)       // LFO Sync = 1 bar (16 steps over four beats = 1/16 steps)
SP( 37, 5)       // LFO Shape = Custom
SP( 38, 8.f)     // Glide
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.45f)   // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 7)       // M2 Dst = A Pitch
SP( 73, 0.625f)  // M2 Amt — one hex digit = one semitone
SP( 74, 1)       // M3 Src = LFO1
SP( 75, 1)       // M3 Dst = A Pos
SP( 76, 0.3f)    // M3 Amt
SP( 98, 1)       // Voice Mode = Mono
SPLFO(1, "0479047c0479047e", 0.f)
SP_END()

// LD Cry Wheel — a vocal lead whose wheel is intensity, not depth: it opens
// the vowel, blows breath into it and quickens the vibrato, the last of those
// only at the top of the travel. Macro 1: opens the filter (the cry).
SP_PRESET("LD Cry Wheel")
SP(  0, 6)       // A Table = Vox
SP(  1, 0.3f)    // A Position
SP( 18, 5200.f)  // Cutoff
SP( 19, 0.3f)    // Resonance
SP( 26, 0.85f)   // Sustain
SP( 27, 220.f)   // Release
SP( 32, 4.5f)    // LFO Rate
SP( 36, 9.f)     // LFO>Pitch
SP( 38, 60.f)    // Glide
SP( 41, 0.35f)   // Env2>Position
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 14)      // M2 Src = Mod Wheel
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.55f)   // M2 Amt
SP( 74, 14)      // M3 Src = Mod Wheel
SP( 75, 10)      // M3 Dst = Noise Level
SP( 76, 0.25f)   // M3 Amt
SP( 77, 14)      // M4 Src = Mod Wheel
SP( 78, 17)      // M4 Dst = LFO1 Rate
SP( 79, 0.3f)    // M4 Amt
SP( 98, 2)       // Voice Mode = Legato
SP(104, 1)       // M4 Curve = Exp
SP_END()

// LD Curve Sync — a hard-sync sweep with a shaped hand on it: ENV3 rakes the
// sync ratio through an S-curve, so the sweep eases in and eases out instead
// of raking linearly. Macro 1: raises the sync floor (A Warp Amt).
SP_PRESET("LD Curve Sync")
SP(  1, 0.1f)    // A Position
SP( 18, 9500.f)  // Cutoff
SP( 21, 6.f)     // Drive
SP( 26, 0.8f)    // Sustain
SP( 27, 140.f)   // Release
SP( 38, 20.f)    // Glide
SP( 48, 1)       // A Warp = Sync
SP( 49, 0.2f)    // A Warp Amt
SP( 63, 600.f)   // E3 Decay
SP( 64, 0.15f)   // E3 Sustain
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 3)       // M1 Dst = A Warp Amt
SP( 70, 0.45f)   // M1 Amt
SP( 71, 5)       // M2 Src = ENV3
SP( 72, 3)       // M2 Dst = A Warp Amt
SP( 73, 0.55f)   // M2 Amt
SP( 74, 6)       // M3 Src = Velocity
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.3f)    // M3 Amt
SP( 98, 1)       // Voice Mode = Mono
SP(101, 1)       // M1 Curve = Exp
SP(102, 2)       // M2 Curve = S-curve
SP_END()

// LD Gate Runner — two grids at once: a 1/16 gate on the level and a slower
// two-bar grid walking the wavetable frame, so the rhythm and the timbre run
// at different lengths. Macro 1: opens the filter.
SP_PRESET("LD Gate Runner")
SP(  1, 0.2f)    // A Position
SP(  4, 0.1f)    // A Level — a little bleed under the gate
SP(  5, 3)       // A Unison
SP(  6, 18.f)    // A Detune
SP( 18, 8000.f)  // Cutoff
SP( 19, 0.25f)   // Resonance
SP( 26, 1.f)     // Sustain
SP( 27, 150.f)   // Release
SP( 33, 3)       // LFO Sync = 1 bar
SP( 37, 5)       // LFO Shape = Custom
SP( 55, 2)       // L2 Sync = 2 bars
SP( 56, 5)       // L2 Shape = Custom
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 5)       // M2 Dst = A Level
SP( 73, 0.9f)    // M2 Amt
SP( 74, 2)       // M3 Src = LFO2
SP( 75, 1)       // M3 Dst = A Pos
SP( 76, 0.6f)    // M3 Amt
SPLFO(1, "fc8f4cf8a0f6c840", 0.03f)
SPLFO(2, "258bfcb852147ade", 0.4f)
SP_END()

// LD Ribbon CC — one learned knob (CC 74) worth reaching for: it opens the
// filter on an S-curve, fans the unison and pushes the drive, all at once.
// Macro 1: opens the filter (a second hand on the same sound).
SP_PRESET("LD Ribbon CC")
SP(  1, 0.15f)   // A Position
SP(  5, 5)       // A Unison
SP(  6, 10.f)    // A Detune
SP(  7, 0.75f)   // A Spread
SP( 18, 4000.f)  // Cutoff
SP( 19, 0.3f)    // Resonance
SP( 26, 0.85f)   // Sustain
SP( 27, 200.f)   // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 16)      // M2 Src = MIDI CC
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.55f)   // M2 Amt
SP( 74, 16)      // M3 Src = MIDI CC
SP( 75, 14)      // M3 Dst = A Detune
SP( 76, 0.5f)    // M3 Amt
SP( 77, 16)      // M4 Src = MIDI CC
SP( 78, 13)      // M4 Dst = Drive
SP( 79, 0.4f)    // M4 Amt
SP(102, 2)       // M2 Curve = S-curve
SP(103, 1)       // M3 Curve = Exp
SP(104, 1)       // M4 Curve = Exp
SPCC(74)
SP_END()

// LD Solar Import — both oscillators are imported: the vowel table sings the
// line while the metal comb rings a fifth above it, bloomed in by a one-shot
// LFO3. Macro 1: walks the vowel (A Position).
SP_PRESET("LD Solar Import")
SP(  0, 8)       // A Table = custom slot 8 (formant vowels)
SP(  1, 0.2f)    // A Position
SP(  5, 3)       // A Unison
SP(  6, 16.f)    // A Detune
SP(  8, 8)       // B Table = custom slot 8 (metal comb)
SP(  9, 0.6f)    // B Position
SP( 10, 7)       // B Coarse
SP( 12, 0.35f)   // B Level
SP( 18, 7000.f)  // Cutoff
SP( 19, 0.2f)    // Resonance
SP( 24, 6.f)     // Attack
SP( 26, 0.85f)   // Sustain
SP( 27, 300.f)   // Release
SP( 57, 1.2f)    // L3 Rate
SP( 61, 1)       // L3 Mode = One-shot
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 1)       // M1 Dst = A Pos
SP( 70, 0.6f)    // M1 Amt
SP( 71, 3)       // M2 Src = LFO3
SP( 72, 6)       // M2 Dst = B Level
SP( 73, 0.35f)   // M2 Amt
SP( 74, 6)       // M3 Src = Velocity
SP( 75, 2)       // M3 Dst = B Pos
SP( 76, 0.3f)    // M3 Amt
SPWTA("5c0d37c1e4244576")
SPWTB("c452993b482418f3")
SP_END()

// LD Spark Bloom — a one-shot sine LFO2 is the second envelope: it lifts the
// filter and the frame over 300 ms, dips under the base, and settles back to
// nothing. Macro 1: opens the filter.
SP_PRESET("LD Spark Bloom")
SP(  1, 0.4f)    // A Position
SP(  5, 3)       // A Unison
SP(  6, 14.f)    // A Detune
SP( 18, 1600.f)  // Cutoff
SP( 19, 0.45f)   // Resonance
SP( 21, 4.f)     // Drive
SP( 25, 900.f)   // Decay
SP( 26, 0.65f)   // Sustain
SP( 27, 180.f)   // Release
SP( 54, 1.6f)    // L2 Rate
SP( 60, 1)       // L2 Mode = One-shot
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.6f)    // M2 Amt
SP( 74, 2)       // M3 Src = LFO2
SP( 75, 1)       // M3 Dst = A Pos
SP( 76, 0.25f)   // M3 Amt
SP(102, 1)       // M2 Curve = Exp
SP(103, 2)       // M3 Curve = S-curve
SP_END()

// LD Stair Climb — the grid as an envelope: LFO1 is Custom AND one-shot, so
// every note climbs its sixteen steps once, in 400 ms, and holds the top.
// Macro 1: opens the filter.
SP_PRESET("LD Stair Climb")
SP(  0, 5)       // A Table = Digital
SP(  1, 0.5f)    // A Position
SP( 18, 10000.f) // Cutoff
SP( 25, 800.f)   // Decay
SP( 26, 0.6f)    // Sustain
SP( 27, 100.f)   // Release
SP( 32, 2.5f)    // LFO Rate — the whole sixteen-step climb in 400 ms
SP( 37, 5)       // LFO Shape = Custom
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 7)       // M2 Dst = A Pitch
SP( 73, 0.625f)  // M2 Amt — one hex digit = one semitone
SP( 74, 6)       // M3 Src = Velocity
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.3f)    // M3 Amt
SP(100, 1)       // L1 Mode = One-shot
SPLFO(1, "024579bcefecb975", 0.05f)
SP_END()

// ---------------------------------------------------------- Pad (8), bank 2

// PD Bloom Curve — a four-second one-shot sine on LFO2 opens the pad long
// after the amp envelope has settled, squared by an Exp curve so the bloom
// starts slowly. Macro 1: opens the filter.
SP_PRESET("PD Bloom Curve")
SP(  1, 0.25f)   // A Position
SP(  5, 5)       // A Unison
SP(  6, 26.f)    // A Detune
SP(  7, 0.95f)   // A Spread
SP( 18, 900.f)   // Cutoff
SP( 19, 0.2f)    // Resonance
SP( 24, 800.f)   // Attack
SP( 26, 0.9f)    // Sustain
SP( 27, 2500.f)  // Release
SP( 54, 0.25f)   // L2 Rate — one shot lasts four seconds
SP( 60, 1)       // L2 Mode = One-shot
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.45f)   // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.65f)   // M2 Amt
SP( 74, 2)       // M3 Src = LFO2
SP( 75, 1)       // M3 Dst = A Pos
SP( 76, 0.3f)    // M3 Amt
SP(102, 1)       // M2 Curve = Exp
SP_END()

// PD Choir Import — the imported vowel table as a six-voice choir, with
// keytracked breath on the velocity and a glacial LFO3 drifting the vowel.
// Macro 1: walks the vowel, u through i (A Position).
SP_PRESET("PD Choir Import")
SP(  0, 8)       // A Table = custom slot 8 (formant vowels)
SP(  1, 0.1f)    // A Position
SP(  5, 6)       // A Unison
SP(  6, 24.f)    // A Detune
SP(  7, 1.f)     // A Spread
SP( 16, 0.06f)   // Noise
SP( 18, 6000.f)  // Cutoff
SP( 24, 900.f)   // Attack
SP( 26, 0.9f)    // Sustain
SP( 27, 2600.f)  // Release
SP( 44, 0.5f)    // Noise Color
SP( 45, 1)       // Noise Track = On
SP( 57, 0.05f)   // L3 Rate
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 1)       // M1 Dst = A Pos
SP( 70, 0.7f)    // M1 Amt
SP( 71, 3)       // M2 Src = LFO3
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.25f)   // M2 Amt
SP( 74, 6)       // M3 Src = Velocity
SP( 75, 10)      // M3 Dst = Noise Level
SP( 76, 0.2f)    // M3 Amt
SPWTA("5c0d37c1e4244576")
SP_END()

// PD Dawn Rise — an eight-second sunrise per note: a one-shot saw on LFO3
// starts below the base cutoff and climbs past it, the frame following on an
// S-curve. Macro 1: opens the filter.
SP_PRESET("PD Dawn Rise")
SP(  1, 0.15f)   // A Position
SP(  5, 5)       // A Unison
SP(  6, 22.f)    // A Detune
SP(  7, 0.9f)    // A Spread
SP( 18, 600.f)   // Cutoff
SP( 19, 0.18f)   // Resonance
SP( 24, 1500.f)  // Attack
SP( 26, 0.95f)   // Sustain
SP( 27, 3500.f)  // Release
SP( 57, 0.12f)   // L3 Rate — one shot lasts 8.3 s
SP( 59, 2)       // L3 Shape = Saw
SP( 61, 1)       // L3 Mode = One-shot
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 3)       // M2 Src = LFO3
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.5f)    // M2 Amt
SP( 74, 3)       // M3 Src = LFO3
SP( 75, 1)       // M3 Dst = A Pos
SP( 76, 0.35f)   // M3 Amt
SP(103, 2)       // M3 Curve = S-curve
SP_END()

// PD Drift Steps — a sequenced pad that never clicks: a four-bar grid of
// quarter-note steps with the smooth almost fully up, so the levels glide
// rather than jump. Macro 1: opens the filter.
SP_PRESET("PD Drift Steps")
SP(  1, 0.2f)    // A Position
SP(  5, 4)       // A Unison
SP(  6, 20.f)    // A Detune
SP(  8, 6)       // B Table = Vox
SP(  9, 0.4f)    // B Position
SP( 12, 0.4f)    // B Level
SP( 18, 2400.f)  // Cutoff
SP( 24, 1200.f)  // Attack
SP( 26, 0.9f)    // Sustain
SP( 27, 3000.f)  // Release
SP( 33, 1)       // LFO Sync = 4 bars (16 steps over sixteen beats = 1/4 steps)
SP( 37, 5)       // LFO Shape = Custom
SP( 58, 2)       // L3 Sync = 2 bars
SP( 59, 5)       // L3 Shape = Custom
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.45f)   // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.5f)    // M2 Amt
SP( 74, 3)       // M3 Src = LFO3
SP( 75, 2)       // M3 Dst = B Pos
SP( 76, 0.5f)    // M3 Amt
SPLFO(1, "369cfda8641259bd", 0.75f)
SPLFO(3, "8befdb86423579ce", 0.6f)
SP_END()

// PD Halo Wheel — one wheel, three destinations, all S-curved: the pad walks
// from dark and narrow to a wide bell-lit halo, and the change happens in the
// middle of the travel where a hand can feel it. Macro 1: opens the filter.
SP_PRESET("PD Halo Wheel")
SP(  1, 0.2f)    // A Position
SP(  5, 5)       // A Unison
SP(  6, 20.f)    // A Detune
SP(  7, 0.9f)    // A Spread
SP(  8, 4)       // B Table = Bell
SP(  9, 0.4f)    // B Position
SP( 10, 12)      // B Coarse
SP( 13, 3)       // B Unison
SP( 18, 1800.f)  // Cutoff
SP( 24, 1000.f)  // Attack
SP( 26, 0.9f)    // Sustain
SP( 27, 2800.f)  // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 14)      // M2 Src = Mod Wheel
SP( 72, 6)       // M2 Dst = B Level
SP( 73, 0.6f)    // M2 Amt
SP( 74, 14)      // M3 Src = Mod Wheel
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.45f)   // M3 Amt
SP( 77, 14)      // M4 Src = Mod Wheel
SP( 78, 14)      // M4 Dst = A Detune
SP( 79, 0.3f)    // M4 Amt
SP(102, 2)       // M2 Curve = S-curve
SP(103, 2)       // M3 Curve = S-curve
SP_END()

// PD Pulse Weave — two grids of different lengths weave the two oscillators'
// frames against each other, one bar against one half note, so the pattern
// only repeats every fourth bar. Macro 1: opens the filter.
SP_PRESET("PD Pulse Weave")
SP(  1, 0.1f)    // A Position
SP(  5, 4)       // A Unison
SP(  6, 18.f)    // A Detune
SP(  8, 2)       // B Table = Harmonic
SP(  9, 0.2f)    // B Position
SP( 12, 0.45f)   // B Level
SP( 13, 3)       // B Unison
SP( 18, 3200.f)  // Cutoff
SP( 24, 700.f)   // Attack
SP( 26, 0.9f)    // Sustain
SP( 27, 2400.f)  // Release
SP( 33, 3)       // LFO Sync = 1 bar
SP( 37, 5)       // LFO Shape = Custom
SP( 55, 4)       // L2 Sync = 1/2
SP( 56, 5)       // L2 Shape = Custom
SP( 57, 0.07f)   // L3 Rate
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.45f)   // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.55f)   // M2 Amt
SP( 74, 2)       // M3 Src = LFO2
SP( 75, 2)       // M3 Dst = B Pos
SP( 76, 0.5f)    // M3 Amt
SP( 77, 3)       // M4 Src = LFO3
SP( 78, 11)      // M4 Dst = Cutoff
SP( 79, 0.2f)    // M4 Amt
SPLFO(1, "258bfda8520147ac", 0.2f)
SPLFO(2, "c9630369cfc96303", 0.45f)
SP_END()

// PD Silk Curve — the crossfade IS the patch: Macro 1 takes the saw layer out
// and the vocal layer in through two S-curves, so the middle of the knob is a
// true blend and neither end dips. Macro 1: crossfades the two layers.
SP_PRESET("PD Silk Curve")
SP(  1, 0.3f)    // A Position
SP(  5, 5)       // A Unison
SP(  6, 22.f)    // A Detune
SP(  7, 0.9f)    // A Spread
SP(  8, 6)       // B Table = Vox
SP(  9, 0.5f)    // B Position
SP( 13, 4)       // B Unison
SP( 14, 20.f)    // B Detune
SP( 18, 4000.f)  // Cutoff
SP( 24, 900.f)   // Attack
SP( 26, 0.9f)    // Sustain
SP( 27, 2500.f)  // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 5)       // M1 Dst = A Level
SP( 70, -0.8f)   // M1 Amt
SP( 71, 9)       // M2 Src = Macro 1
SP( 72, 6)       // M2 Dst = B Level
SP( 73, 0.8f)    // M2 Amt
SP( 74, 9)       // M3 Src = Macro 1
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.25f)   // M3 Amt
SP(101, 2)       // M1 Curve = S-curve
SP(102, 2)       // M2 Curve = S-curve
SP_END()

// PD Vapor Bend — the pitch wheel bends the timbre as well as the pitch: five
// semitones of scoop, with the frame and the unison spread following it on an
// S-curve. Macro 1: opens the filter.
SP_PRESET("PD Vapor Bend")
SP(  1, 0.2f)    // A Position
SP(  5, 5)       // A Unison
SP(  6, 25.f)    // A Detune
SP(  7, 1.f)     // A Spread
SP( 18, 2600.f)  // Cutoff
SP( 19, 0.2f)    // Resonance
SP( 24, 800.f)   // Attack
SP( 26, 0.9f)    // Sustain
SP( 27, 2600.f)  // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 15)      // M2 Src = Pitch Bend
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.4f)    // M2 Amt
SP( 74, 15)      // M3 Src = Pitch Bend
SP( 75, 14)      // M3 Dst = A Detune
SP( 76, 0.35f)   // M3 Amt
SP( 99, 5)       // Bend Range = 5 st
SP(102, 2)       // M2 Curve = S-curve
SP_END()

// --------------------------------------------------------- Keys (7), bank 2

// KY Amber Import — the imported drift table as an electric piano: frame 0 is
// hollow and odd-harmonic, and only a hard strike fills the evens in, because
// the velocity slot is squared. Macro 1: fills the harmonics (A Position).
SP_PRESET("KY Amber Import")
SP(  0, 8)       // A Table = custom slot 8 (harmonic drift)
SP(  1, 0.1f)    // A Position
SP( 18, 6500.f)  // Cutoff
SP( 25, 1600.f)  // Decay
SP( 26, 0.35f)   // Sustain
SP( 27, 450.f)   // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 1)       // M1 Dst = A Pos
SP( 70, 0.5f)    // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.45f)   // M2 Amt
SP( 74, 6)       // M3 Src = Velocity
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.3f)    // M3 Amt
SP(102, 1)       // M2 Curve = Exp
SPWTA("33f9d0639a00d4f0")
SP_END()

// KY Chime Steps — held chords shimmer on their own: a half-note grid with a
// light smooth walks the bell frame in a sixteen-step figure under the keys.
// Macro 1: opens the filter.
SP_PRESET("KY Chime Steps")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.3f)    // A Position
SP( 18, 8000.f)  // Cutoff
SP( 25, 1500.f)  // Decay
SP( 26, 0.4f)    // Sustain
SP( 27, 700.f)   // Release
SP( 55, 4)       // L2 Sync = 1/2
SP( 56, 5)       // L2 Shape = Custom
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.35f)   // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.5f)    // M2 Amt
SP( 74, 6)       // M3 Src = Velocity
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.3f)    // M3 Amt
SPLFO(2, "4af6c28e4af6c28e", 0.15f)
SP_END()

// KY Curve Touch — three curves and no tricks: velocity squared into the
// drive so only a hard strike barks, velocity squared into the cutoff, and
// aftertouch S-curved into the frame. Macro 1: opens the filter (Exp).
SP_PRESET("KY Curve Touch")
SP(  0, 2)       // A Table = Harmonic
SP(  1, 0.2f)    // A Position
SP( 18, 3000.f)  // Cutoff
SP( 21, 2.f)     // Drive
SP( 25, 1300.f)  // Decay
SP( 26, 0.45f)   // Sustain
SP( 27, 350.f)   // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 13)      // M2 Dst = Drive
SP( 73, 0.5f)    // M2 Amt
SP( 74, 8)       // M3 Src = Aftertouch
SP( 75, 1)       // M3 Dst = A Pos
SP( 76, 0.5f)    // M3 Amt
SP( 77, 6)       // M4 Src = Velocity
SP( 78, 11)      // M4 Dst = Cutoff
SP( 79, 0.35f)   // M4 Amt
SP(101, 1)       // M1 Curve = Exp
SP(102, 1)       // M2 Curve = Exp
SP(103, 2)       // M3 Curve = S-curve
SP_END()

// KY Hammer One — two one-shot LFOs make the hammer: a 28 ms square bark at
// the strike that then holds negative like a damper, and a slow sine bloom
// walking the frame behind it. Macro 1: opens the filter.
SP_PRESET("KY Hammer One")
SP(  0, 2)       // A Table = Harmonic
SP(  1, 0.3f)    // A Position
SP( 18, 3400.f)  // Cutoff
SP( 19, 0.2f)    // Resonance
SP( 25, 1200.f)  // Decay
SP( 26, 0.4f)    // Sustain
SP( 27, 300.f)   // Release
SP( 54, 18.f)    // L2 Rate — the bark is the first half cycle, 28 ms
SP( 56, 3)       // L2 Shape = Square
SP( 57, 2.2f)    // L3 Rate
SP( 60, 1)       // L2 Mode = One-shot
SP( 61, 1)       // L3 Mode = One-shot
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.45f)   // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 13)      // M2 Dst = Drive
SP( 73, 0.4f)    // M2 Amt
SP( 74, 3)       // M3 Src = LFO3
SP( 75, 1)       // M3 Dst = A Pos
SP( 76, 0.3f)    // M3 Amt
SP( 77, 6)       // M4 Src = Velocity
SP( 78, 13)      // M4 Dst = Drive
SP( 79, 0.35f)   // M4 Amt
SP_END()

// KY Reed Wheel — the wheel is the reed's effort: it pushes the drive and
// quickens the tremolo, the latter only at the top of the travel.
// Macro 1: adds bark (Drive).
SP_PRESET("KY Reed Wheel")
SP(  0, 2)       // A Table = Harmonic
SP(  1, 0.08f)   // A Position
SP( 18, 3000.f)  // Cutoff
SP( 21, 4.f)     // Drive
SP( 25, 1500.f)  // Decay
SP( 26, 0.4f)    // Sustain
SP( 27, 320.f)   // Release
SP( 32, 4.f)     // LFO Rate
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 13)      // M1 Dst = Drive
SP( 70, 0.4f)    // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 5)       // M2 Dst = A Level
SP( 73, 0.14f)   // M2 Amt
SP( 74, 14)      // M3 Src = Mod Wheel
SP( 75, 17)      // M3 Dst = LFO1 Rate
SP( 76, 0.35f)   // M3 Amt
SP( 77, 14)      // M4 Src = Mod Wheel
SP( 78, 13)      // M4 Dst = Drive
SP( 79, 0.45f)   // M4 Amt
SP(103, 1)       // M3 Curve = Exp
SP_END()

// KY Stack Bloom — a DX tine that swells: a one-shot sine on LFO2 opens the FM
// depth over the first 350 ms and settles back, velocity squared on top.
// Macro 1: tine brightness (FM depth).
SP_PRESET("KY Stack Bloom")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.2f)    // A Position
SP(  8, 2)       // B Table = Harmonic
SP( 10, 12)      // B Coarse
SP( 18, 7000.f)  // Cutoff
SP( 25, 1700.f)  // Decay
SP( 26, 0.35f)   // Sustain
SP( 27, 420.f)   // Release
SP( 48, 6)       // A Warp = FM
SP( 49, 0.12f)   // A Warp Amt
SP( 54, 1.4f)    // L2 Rate
SP( 60, 1)       // L2 Mode = One-shot
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 3)       // M1 Dst = A Warp Amt
SP( 70, 0.35f)   // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 3)       // M2 Dst = A Warp Amt
SP( 73, 0.3f)    // M2 Amt
SP( 74, 6)       // M3 Src = Velocity
SP( 75, 3)       // M3 Dst = A Warp Amt
SP( 76, 0.3f)    // M3 Amt
SP(103, 1)       // M3 Curve = Exp
SP_END()

// KY Wire Clav — a clav with a chop built in: a half-note grid of sixteen
// hard steps dips the level between the notes while ENV2 quacks the filter.
// Macro 1: wah (opens the filter).
SP_PRESET("KY Wire Clav")
SP(  0, 5)       // A Table = Digital
SP(  1, 0.4f)    // A Position
SP(  4, 0.45f)   // A Level
SP( 18, 2200.f)  // Cutoff
SP( 19, 0.5f)    // Resonance
SP( 21, 3.f)     // Drive
SP( 22, 0.6f)    // Env2>Cutoff
SP( 23, 0.45f)   // Keytrack
SP( 25, 380.f)   // Decay
SP( 26, 0.3f)    // Sustain
SP( 27, 110.f)   // Release
SP( 29, 150.f)   // E2 Decay
SP( 33, 4)       // LFO Sync = 1/2
SP( 37, 5)       // LFO Shape = Custom
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 5)       // M2 Dst = A Level
SP( 73, 0.5f)    // M2 Amt
SP( 74, 6)       // M3 Src = Velocity
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.35f)   // M3 Amt
SPLFO(1, "fbf7fbf3fbf7fdf5", 0.04f)
SP_END()

// -------------------------------------------------------- Pluck (6), bank 2

// PL Comb Metal — the imported metal comb struck and left to ring: ENV2 walks
// the comb from gong to bar chime as the note decays, each hit landing
// somewhere new in the stereo field. Macro 1: walks the comb (A Position).
SP_PRESET("PL Comb Metal")
SP(  0, 8)       // A Table = custom slot 8 (metal comb)
SP(  1, 0.25f)   // A Position
SP( 18, 9000.f)  // Cutoff
SP( 25, 650.f)   // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 700.f)   // Release
SP( 41, 0.4f)    // Env2>Position
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 1)       // M1 Dst = A Pos
SP( 70, 0.55f)   // M1 Amt
SP( 71, 13)      // M2 Src = Random
SP( 72, 16)      // M2 Dst = Pan
SP( 73, 0.7f)    // M2 Amt
SP( 74, 6)       // M3 Src = Velocity
SP( 75, 1)       // M3 Dst = A Pos
SP( 76, 0.3f)    // M3 Amt
SPWTA("c452993b482418f3")
SP_END()

// PL Curve Snap — the snap comes from the curve: ENV3 is squared into both
// the fold frame and the drive, so the flick at the attack is twice as sharp
// as the envelope itself. Macro 1: raises the fold floor (A Position).
SP_PRESET("PL Curve Snap")
SP(  0, 7)       // A Table = Fold
SP(  1, 0.1f)    // A Position
SP( 18, 9000.f)  // Cutoff
SP( 23, 0.4f)    // Keytrack
SP( 25, 420.f)   // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 500.f)   // Release
SP( 63, 140.f)   // E3 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 1)       // M1 Dst = A Pos
SP( 70, 0.35f)   // M1 Amt
SP( 71, 5)       // M2 Src = ENV3
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.7f)    // M2 Amt
SP( 74, 5)       // M3 Src = ENV3
SP( 75, 13)      // M3 Dst = Drive
SP( 76, 0.4f)    // M3 Amt
SP(102, 1)       // M2 Curve = Exp
SP(103, 1)       // M3 Curve = Exp
SP_END()

// PL Drop Bell — a bell dropped onto its note: a one-shot saw falls two
// octaves in 125 ms, the A Coarse offset making the landing exact, and every
// hit scatters. Macro 1: opens the filter.
SP_PRESET("PL Drop Bell")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.45f)   // A Position
SP(  2, 12)      // A Coarse — centres the bipolar one-shot on the played note
SP( 18, 8000.f)  // Cutoff
SP( 25, 500.f)   // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 600.f)   // Release
SP( 54, 8.f)     // L2 Rate — the drop takes 125 ms
SP( 56, 2)       // L2 Shape = Saw
SP( 60, 1)       // L2 Mode = One-shot
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.35f)   // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 7)       // M2 Dst = A Pitch
SP( 73, -0.5f)   // M2 Amt — +12 st at note-on falling to -12 st
SP( 74, 13)      // M3 Src = Random
SP( 75, 16)      // M3 Dst = Pan
SP( 76, 0.5f)    // M3 Amt
SP_END()

// PL Grid Harp — one note, one run: a one-shot Custom LFO1 plays a sixteen-
// step harp figure up and back to the root in 450 ms, a digit a semitone.
// Macro 1: opens the filter.
SP_PRESET("PL Grid Harp")
SP(  1, 0.7f)    // A Position
SP( 18, 7000.f)  // Cutoff
SP( 23, 0.5f)    // Keytrack
SP( 25, 600.f)   // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 800.f)   // Release
SP( 32, 2.2f)    // LFO Rate — the whole run in 455 ms
SP( 37, 5)       // LFO Shape = Custom
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 7)       // M2 Dst = A Pitch
SP( 73, 0.625f)  // M2 Amt — one hex digit = one semitone
SP( 74, 6)       // M3 Src = Velocity
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.3f)    // M3 Amt
SP(100, 1)       // L1 Mode = One-shot
SPLFO(1, "02579cefca975320", 0.02f)
SP_END()

// PL Ratchet Pick — the grid at a quarter note puts all sixteen steps inside
// one beat, so the level pattern is a burst of 1/64 ratchets rather than a
// groove. Macro 1: opens the filter.
SP_PRESET("PL Ratchet Pick")
SP(  1, 0.35f)   // A Position
SP(  4, 0.f)     // A Level — the grid is the level
SP( 18, 3500.f)  // Cutoff
SP( 19, 0.35f)   // Resonance
SP( 22, 0.5f)    // Env2>Cutoff
SP( 25, 300.f)   // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 220.f)   // Release
SP( 29, 120.f)   // E2 Decay
SP( 33, 5)       // LFO Sync = 1/4 (16 steps inside one beat = 1/64 ratchets)
SP( 37, 5)       // LFO Shape = Custom
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.45f)   // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 5)       // M2 Dst = A Level
SP( 73, 1.f)     // M2 Amt
SP( 74, 6)       // M3 Src = Velocity
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.3f)    // M3 Amt
SPLFO(1, "ff00fc00f8f000c0", 0.f)
SP_END()

// PL Wheel Mute — the wheel is a palm: it closes the filter and pushes the
// drive, both squared, so the mute arrives suddenly at the top of the travel
// like a hand landing. Macro 1: opens the filter.
SP_PRESET("PL Wheel Mute")
SP(  1, 0.6f)    // A Position
SP( 18, 6000.f)  // Cutoff
SP( 19, 0.25f)   // Resonance
SP( 21, 3.f)     // Drive
SP( 23, 0.4f)    // Keytrack
SP( 25, 380.f)   // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 300.f)   // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 14)      // M2 Src = Mod Wheel
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, -0.55f)  // M2 Amt
SP( 74, 14)      // M3 Src = Mod Wheel
SP( 75, 13)      // M3 Dst = Drive
SP( 76, 0.4f)    // M3 Amt
SP(102, 1)       // M2 Curve = Exp
SP(103, 1)       // M3 Curve = Exp
SP_END()

// ----------------------------------------------------------- FX (5), bank 2

// FX Alien Knob — one learned controller (74) is the whole instrument: it
// walks the vowel, folds the phase, bends the pitch and sharpens the
// resonance together. Macro 1: opens the filter.
SP_PRESET("FX Alien Knob")
SP(  0, 3)       // A Table = Formant
SP(  1, 0.3f)    // A Position
SP( 16, 0.1f)    // Noise
SP( 18, 1200.f)  // Cutoff
SP( 19, 0.55f)   // Resonance
SP( 24, 300.f)   // Attack
SP( 26, 1.f)     // Sustain
SP( 27, 1500.f)  // Release
SP( 48, 4)       // A Warp = Mirror
SP( 49, 0.2f)    // A Warp Amt
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 16)      // M2 Src = MIDI CC
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.8f)    // M2 Amt
SP( 74, 16)      // M3 Src = MIDI CC
SP( 75, 3)       // M3 Dst = A Warp Amt
SP( 76, 0.6f)    // M3 Amt
SP( 77, 16)      // M4 Src = MIDI CC
SP( 78, 7)       // M4 Dst = A Pitch
SP( 79, 0.3f)    // M4 Amt
SP( 80, 16)      // M5 Src = MIDI CC
SP( 81, 12)      // M5 Dst = Resonance
SP( 82, 0.35f)   // M5 Amt
SP(102, 2)       // M2 Curve = S-curve
SP(104, 1)       // M4 Curve = Exp
SPCC(74)
SP_END()

// FX Bend Siren — a siren played from the pitch wheel: two octaves of bend
// range with the resonant peak and its Q riding the wheel through an S-curve.
// Macro 1: opens the filter.
SP_PRESET("FX Bend Siren")
SP(  0, 6)       // A Table = Vox
SP(  1, 0.4f)    // A Position
SP( 16, 0.15f)   // Noise
SP( 18, 900.f)   // Cutoff
SP( 19, 0.7f)    // Resonance
SP( 24, 80.f)    // Attack
SP( 26, 1.f)     // Sustain
SP( 27, 1200.f)  // Release
SP( 32, 5.f)     // LFO Rate
SP( 36, 15.f)    // LFO>Pitch
SP( 40, 0.5f)    // Master
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 15)      // M2 Src = Pitch Bend
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.6f)    // M2 Amt
SP( 74, 15)      // M3 Src = Pitch Bend
SP( 75, 12)      // M3 Dst = Resonance
SP( 76, 0.3f)    // M3 Amt
SP( 99, 24)      // Bend Range = 24 st
SP(102, 2)       // M2 Curve = S-curve
SP_END()

// FX Machine Steps — machine speech: a half-note pitch grid against a
// quarter-note level grid, with an S&H LFO3 flickering the resonance so no
// two passes sound alike. Macro 1: opens the filter.
SP_PRESET("FX Machine Steps")
SP(  0, 5)       // A Table = Digital
SP(  1, 0.6f)    // A Position
SP(  4, 0.15f)   // A Level
SP( 16, 0.12f)   // Noise
SP( 18, 2500.f)  // Cutoff
SP( 19, 0.5f)    // Resonance
SP( 26, 1.f)     // Sustain
SP( 27, 150.f)   // Release
SP( 33, 4)       // LFO Sync = 1/2
SP( 37, 5)       // LFO Shape = Custom
SP( 40, 0.45f)   // Master
SP( 55, 5)       // L2 Sync = 1/4
SP( 56, 5)       // L2 Shape = Custom
SP( 57, 9.f)     // L3 Rate
SP( 59, 4)       // L3 Shape = S&H
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.45f)   // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 7)       // M2 Dst = A Pitch
SP( 73, 0.625f)  // M2 Amt — one hex digit = one semitone
SP( 74, 2)       // M3 Src = LFO2
SP( 75, 5)       // M3 Dst = A Level
SP( 76, 0.85f)   // M3 Amt
SP( 77, 3)       // M4 Src = LFO3
SP( 78, 12)      // M4 Dst = Resonance
SP( 79, 0.4f)    // M4 Amt
SPLFO(1, "0c3f7a05c0f3a750", 0.f)
SPLFO(2, "fa0f50c0f0a05000", 0.f)
SP_END()

// FX Rust Import — both imported tables against each other: the metal comb
// ring-modulated by the resonant sweep a fourth below, every strike landing on
// a new frame and a new pan. Macro 1: retunes the modulator (B Pitch).
SP_PRESET("FX Rust Import")
SP(  0, 8)       // A Table = custom slot 8 (metal comb)
SP(  1, 0.5f)    // A Position
SP(  8, 8)       // B Table = custom slot 8 (reso sweep)
SP(  9, 0.3f)    // B Position
SP( 10, -5)      // B Coarse
SP( 16, 0.08f)   // Noise
SP( 18, 6000.f)  // Cutoff
SP( 24, 20.f)    // Attack
SP( 25, 1200.f)  // Decay
SP( 26, 0.25f)   // Sustain
SP( 27, 900.f)   // Release
SP( 40, 1.f)     // Master
SP( 48, 7)       // A Warp = RM
SP( 49, 0.65f)   // A Warp Amt
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 8)       // M1 Dst = B Pitch
SP( 70, 0.35f)   // M1 Amt
SP( 71, 13)      // M2 Src = Random
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.5f)    // M2 Amt
SP( 74, 13)      // M3 Src = Random
SP( 75, 16)      // M3 Dst = Pan
SP( 76, 0.8f)    // M3 Amt
SPWTA("c452993b482418f3")
SPWTB("16b407f0a07b9117")
SP_END()

// FX Zero Gravity — a drone that takes twelve seconds to arrive: two one-shot
// LFOs, a sine walking the fold frame and a saw opening the filter, each note
// its own slow arc. Macro 1: opens the filter.
SP_PRESET("FX Zero Gravity")
SP(  0, 7)       // A Table = Fold
SP(  1, 0.2f)    // A Position
SP(  2, -12)     // A Coarse
SP(  5, 4)       // A Unison
SP(  6, 35.f)    // A Detune
SP(  7, 1.f)     // A Spread
SP( 17, 0.35f)   // Sub
SP( 18, 700.f)   // Cutoff
SP( 19, 0.4f)    // Resonance
SP( 24, 2500.f)  // Attack
SP( 26, 1.f)     // Sustain
SP( 27, 6000.f)  // Release
SP( 40, 1.f)     // Master
SP( 54, 0.15f)   // L2 Rate — one shot lasts 6.7 s
SP( 57, 0.08f)   // L3 Rate — one shot lasts 12.5 s
SP( 59, 2)       // L3 Shape = Saw
SP( 60, 1)       // L2 Mode = One-shot
SP( 61, 1)       // L3 Mode = One-shot
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 2)       // M2 Src = LFO2
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.6f)    // M2 Amt
SP( 74, 3)       // M3 Src = LFO3
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.45f)   // M3 Amt
SP( 77, 2)       // M4 Src = LFO2
SP( 78, 12)      // M4 Dst = Resonance
SP( 79, 0.25f)   // M4 Amt
SP(103, 2)       // M3 Curve = S-curve
SP_END()

// ----------------------------------------------------- Sequence (4), bank 2

// SQ Acid Grid — the acid line as two drawn grids: one gates the level in
// 1/16s, the other walks the resonant cutoff over the same bar, mono glide
// tying the steps together. Macro 1: opens the filter.
SP_PRESET("SQ Acid Grid")
SP(  1, 0.1f)    // A Position
SP(  4, 0.f)     // A Level — the grid is the level
SP( 18, 220.f)   // Cutoff
SP( 19, 0.78f)   // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 8.f)     // Drive
SP( 22, 0.6f)    // Env2>Cutoff
SP( 26, 1.f)     // Sustain
SP( 27, 80.f)    // Release
SP( 29, 140.f)   // E2 Decay
SP( 33, 3)       // LFO Sync = 1 bar
SP( 37, 5)       // LFO Shape = Custom
SP( 38, 35.f)    // Glide
SP( 40, 0.28f)   // Master
SP( 55, 3)       // L2 Sync = 1 bar
SP( 56, 5)       // L2 Shape = Custom
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 5)       // M2 Dst = A Level
SP( 73, 1.f)     // M2 Amt
SP( 74, 2)       // M3 Src = LFO2
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.65f)   // M3 Amt
SP( 98, 1)       // Voice Mode = Mono
SPLFO(1, "f0c0f8c0fc0af080", 0.02f)
SPLFO(2, "39cfda85369cfa62", 0.25f)
SP_END()

// SQ Chord Ratchet — held chords retriggered by the grid: sixteen steps
// inside one beat is a ratchet roll, and a four-bar grid rolls the filter
// underneath it. Macro 1: opens the filter.
SP_PRESET("SQ Chord Ratchet")
SP(  1, 0.25f)   // A Position
SP(  4, 0.05f)   // A Level
SP(  5, 3)       // A Unison
SP(  6, 16.f)    // A Detune
SP( 18, 3000.f)  // Cutoff
SP( 19, 0.3f)    // Resonance
SP( 26, 1.f)     // Sustain
SP( 27, 200.f)   // Release
SP( 33, 5)       // LFO Sync = 1/4 (16 steps inside one beat)
SP( 37, 5)       // LFO Shape = Custom
SP( 55, 1)       // L2 Sync = 4 bars
SP( 56, 5)       // L2 Shape = Custom
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 5)       // M2 Dst = A Level
SP( 73, 0.95f)   // M2 Amt
SP( 74, 2)       // M3 Src = LFO2
SP( 75, 11)      // M3 Dst = Cutoff
SP( 76, 0.5f)    // M3 Amt
SPLFO(1, "ff88ffc0f8f0c080", 0.01f)
SPLFO(2, "258bfdb8630258bd", 0.5f)
SP_END()

// SQ Ladder Walk — a minor-pentatonic walk drawn into LFO1 at 1/16, one hex
// digit per semitone, with the fixed LFO>Cutoff routing taking the same grid
// as a one-sided filter lift. Macro 1: opens the filter.
SP_PRESET("SQ Ladder Walk")
SP(  1, 0.2f)    // A Position
SP( 18, 1600.f)  // Cutoff
SP( 19, 0.55f)   // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 5.f)     // Drive
SP( 22, 0.5f)    // Env2>Cutoff
SP( 25, 280.f)   // Decay
SP( 26, 0.5f)    // Sustain
SP( 27, 90.f)    // Release
SP( 29, 150.f)   // E2 Decay
SP( 33, 3)       // LFO Sync = 1 bar (16 steps over four beats = 1/16 steps)
SP( 35, 0.3f)    // LFO>Cutoff
SP( 37, 5)       // LFO Shape = Custom
SP( 38, 12.f)    // Glide
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 7)       // M2 Dst = A Pitch
SP( 73, 0.625f)  // M2 Amt — one hex digit = one semitone
SP( 98, 1)       // Voice Mode = Mono
SPLFO(1, "0357a7530357ac75", 0.f)
SP_END()

// SQ Wheel Gate — a gate you can dissolve: the grid is the level, and the mod
// wheel raises its floor until the gaps fill in and the pad runs continuous.
// Macro 1: opens the filter.
SP_PRESET("SQ Wheel Gate")
SP(  1, 0.15f)   // A Position
SP(  4, 0.f)     // A Level — the grid is the level
SP(  5, 5)       // A Unison
SP(  6, 24.f)    // A Detune
SP(  7, 0.95f)   // A Spread
SP( 18, 4000.f)  // Cutoff
SP( 19, 0.25f)   // Resonance
SP( 24, 8.f)     // Attack
SP( 26, 1.f)     // Sustain
SP( 27, 400.f)   // Release
SP( 33, 3)       // LFO Sync = 1 bar
SP( 37, 5)       // LFO Shape = Custom
SP( 58, 3)       // L3 Sync = 1 bar
SP( 59, 5)       // L3 Shape = Custom
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.45f)   // M1 Amt
SP( 71, 1)       // M2 Src = LFO1
SP( 72, 5)       // M2 Dst = A Level
SP( 73, 0.9f)    // M2 Amt
SP( 74, 14)      // M3 Src = Mod Wheel
SP( 75, 5)       // M3 Dst = A Level
SP( 76, 0.85f)   // M3 Amt
SP( 77, 3)       // M4 Src = LFO3
SP( 78, 1)       // M4 Dst = A Pos
SP( 79, 0.5f)    // M4 Amt
SP(103, 2)       // M3 Curve = S-curve
SPLFO(1, "fcf0c8f4f0c8f460", 0.03f)
SPLFO(3, "147adfda8520147a", 0.6f)
SP_END()

// ===========================================================================
// Bank 3 — the arpeggiator (v4). 24 presets, appended; rows above are byte-
// identical. Every row here sets SP(109, 1) itself: SPARP is a STATE macro and
// a state macro never sets a parameter. The step row is the star — ties hold a
// note across steps, the -2..+2 octave column walks the line, and a step that
// is off is a rest that still advances the pattern. Categories in contract
// order, alphabetical within each; only non-default values are listed, so an
// unwritten Arp Sync is 7 (1/16) and an unwritten Arp Retrig is 1.
// ===========================================================================

// --------------------------------------------------------- Bass (6), bank 3

// BA Chug Tie — a tied bassline rather than a chord sweep: two roots held
// across three steps each bar, short chugs between. Macro 1: opens the filter.
SP_PRESET("BA Chug Tie")
SP(  1, 0.22f)   // A Position
SP( 17, 0.45f)   // Sub
SP( 18, 260.f)   // Cutoff
SP( 19, 0.55f)   // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 9.f)     // Drive
SP( 22, 0.5f)    // Env2>Cutoff
SP( 24, 0.5f)    // Attack
SP( 25, 220.f)   // Decay
SP( 26, 0.45f)   // Sustain
SP( 27, 70.f)    // Release
SP( 29, 140.f)   // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.55f)   // M1 Amt
SP( 98, 1)       // Voice Mode = Mono
SP(109, 1)       // Arp On
SP(115, 42.f)    // Arp Gate — short chugs; the ties do the holding
SP(119, 1)       // Arp Vel Mode = Fixed
SP(120, 104)     // Arp Fixed Vel
SPARP("ffffffffffffffff", "05151505051505050515150505150505")
SP_END()

// BA Octave Walk — the octave column is the bassline: the pattern drops and
// lifts an octave step by step so the line walks. Macro 1: opens the filter.
SP_PRESET("BA Octave Walk")
SP(  1, 0.18f)   // A Position
SP( 17, 0.35f)   // Sub
SP( 18, 320.f)   // Cutoff
SP( 19, 0.6f)    // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 7.f)     // Drive
SP( 22, 0.45f)   // Env2>Cutoff
SP( 24, 0.5f)    // Attack
SP( 25, 180.f)   // Decay
SP( 26, 0.3f)    // Sustain
SP( 27, 60.f)    // Release
SP( 29, 120.f)   // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.3f)    // M2 Amt
SP( 98, 1)       // Voice Mode = Mono
SP(109, 1)       // Arp On
SP(115, 58.f)    // Arp Gate
SP(116, 14.f)    // Arp Swing
SP(119, 2)       // Arp Vel Mode = Pattern
SPARP("fac8fac8f9c7fbc9", "05050305070503050505030705030507")
SP_END()

// BA Rest Pocket — rests, not notes: six of the sixteen steps are off, so the
// arp plays a pocket instead of a stream. Macro 1: opens the filter.
SP_PRESET("BA Rest Pocket")
SP(  1, 0.3f)    // A Position
SP( 18, 380.f)   // Cutoff
SP( 19, 0.68f)   // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 10.f)    // Drive
SP( 22, 0.6f)    // Env2>Cutoff
SP( 24, 0.4f)    // Attack
SP( 25, 150.f)   // Decay
SP( 26, 0.2f)    // Sustain
SP( 27, 55.f)    // Release
SP( 29, 110.f)   // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 98, 1)       // Voice Mode = Mono
SP(109, 1)       // Arp On
SP(110, 3)       // Arp Mode = Up-Down Exclusive
SP(115, 36.f)    // Arp Gate
SP(116, 56.f)    // Arp Swing
SP(119, 2)       // Arp Vel Mode = Pattern
SPARP("f0ca0d0bf0c0da0b", "05000505000500050500050005050005")
SP_END()

// BA Sub Thumb — Thumb mode is the point: the thumb note pedals an octave down
// on every other step while the chord answers. Macro 1: opens the filter.
SP_PRESET("BA Sub Thumb")
SP(  1, 0.12f)   // A Position
SP( 17, 0.55f)   // Sub
SP( 18, 300.f)   // Cutoff
SP( 19, 0.4f)    // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 6.f)     // Drive
SP( 24, 0.5f)    // Attack
SP( 25, 200.f)   // Decay
SP( 26, 0.35f)   // Sustain
SP( 27, 65.f)    // Release
SP( 42, 1)       // Sub Shape = triangle
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.55f)   // M1 Amt
SP( 98, 1)       // Voice Mode = Mono
SP(109, 1)       // Arp On
SP(110, 8)       // Arp Mode = Thumb
SP(115, 48.f)    // Arp Gate
SP(119, 2)       // Arp Vel Mode = Pattern
SPARP("f9f9f9f0f9f9f9f0", "03050305030503000305030503050300")
SP_END()

// BA Swing Pump — a hard-swung pump with the latch on: press a chord, let go,
// and the pattern keeps running under the next one. Macro 1: opens the filter.
SP_PRESET("BA Swing Pump")
SP(  1, 0.25f)   // A Position
SP(  5, 3)       // A Unison
SP(  6, 12.f)    // A Detune
SP( 17, 0.3f)    // Sub
SP( 18, 420.f)   // Cutoff
SP( 19, 0.5f)    // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 8.f)     // Drive
SP( 22, 0.5f)    // Env2>Cutoff
SP( 25, 170.f)   // Decay
SP( 26, 0.25f)   // Sustain
SP( 27, 70.f)    // Release
SP( 29, 130.f)   // E2 Decay
SP( 39, 12)      // Voices
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP(109, 1)       // Arp On
SP(110, 2)       // Arp Mode = Up-Down Inclusive
SP(115, 40.f)    // Arp Gate
SP(116, 62.f)    // Arp Swing
SP(117, 1)       // Arp Hold
SPARP("ffffffffffffffff", "05050305050503050505030505050305")
SP_END()

// BA Wide Legato — ties under a gate over 100 %: every note is still sounding
// when the next arrives, so the arp glides. Macro 1: opens the filter.
SP_PRESET("BA Wide Legato")
SP(  1, 0.1f)    // A Position
SP( 17, 0.5f)    // Sub
SP( 18, 340.f)   // Cutoff
SP( 19, 0.35f)   // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 5.f)     // Drive
SP( 24, 4.f)     // Attack
SP( 26, 0.9f)    // Sustain
SP( 27, 180.f)   // Release
SP( 38, 55.f)    // Glide
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 98, 2)       // Voice Mode = Legato
SP(109, 1)       // Arp On
SP(112, 6)       // Arp Sync = 1/8
SP(115, 170.f)   // Arp Gate — over 100 %, so steps overlap and Legato glides
SP(119, 1)       // Arp Vel Mode = Fixed
SP(120, 92)      // Arp Fixed Vel
SPARP("ffffffffffffffff", "05151515051515050315151505150505")
SP_END()

// --------------------------------------------------------- Lead (6), bank 3

// LD Chord Stab — Chord mode is the point: every step fires the whole chord,
// and the rests turn it into a stab rhythm. Macro 1: opens the filter.
SP_PRESET("LD Chord Stab")
SP(  1, 0.4f)    // A Position
SP(  5, 3)       // A Unison
SP(  6, 14.f)    // A Detune
SP(  7, 0.6f)    // A Spread
SP( 18, 3200.f)  // Cutoff
SP( 19, 0.3f)    // Resonance
SP( 21, 4.f)     // Drive
SP( 22, 0.45f)   // Env2>Cutoff
SP( 25, 120.f)   // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 90.f)    // Release
SP( 29, 90.f)    // E2 Decay
SP( 39, 12)      // Voices
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.55f)   // M1 Amt
SP(109, 1)       // Arp On
SP(110, 7)       // Arp Mode = Chord
SP(113, 2)       // Arp Octaves
SP(114, 2)       // Arp Oct Mode = Alternate
SP(115, 30.f)    // Arp Gate
SP(119, 2)       // Arp Vel Mode = Pattern
SPARP("f0d00c0bf00d0a00", "05000500000500050500000500050000")
SP_END()

// LD Leap Runner — the octave column makes it leap: a twelve-step phrase that
// jumps two octaves down and two up inside the bar. Macro 1: opens the filter.
SP_PRESET("LD Leap Runner")
SP(  0, 5)       // A Table = Digital
SP(  1, 0.45f)   // A Position
SP(  5, 2)       // A Unison
SP(  6, 8.f)     // A Detune
SP( 18, 2600.f)  // Cutoff
SP( 19, 0.45f)   // Resonance
SP( 21, 6.f)     // Drive
SP( 22, 0.4f)    // Env2>Cutoff
SP( 25, 140.f)   // Decay
SP( 26, 0.25f)   // Sustain
SP( 27, 80.f)    // Release
SP( 29, 100.f)   // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 98, 1)       // Voice Mode = Mono
SP(109, 1)       // Arp On
SP(115, 52.f)    // Arp Gate
SP(119, 2)       // Arp Vel Mode = Pattern
SP(121, 12)      // Arp Steps — twelve of the sixteen, a 3/4 phrase over 4/4
SPARP("fbd9fcb8fdaefbca", "05070309050107050903050705050505")
SP_END()

// LD Pinky Cry — Pinky mode is the point: the top note answers every other
// step, and the octave column lifts it further. Macro 1: opens the filter.
SP_PRESET("LD Pinky Cry")
SP(  0, 6)       // A Table = Vox
SP(  1, 0.35f)   // A Position
SP(  5, 3)       // A Unison
SP(  6, 10.f)    // A Detune
SP(  7, 0.5f)    // A Spread
SP( 18, 2400.f)  // Cutoff
SP( 19, 0.5f)    // Resonance
SP( 22, 0.35f)   // Env2>Cutoff
SP( 25, 200.f)   // Decay
SP( 26, 0.4f)    // Sustain
SP( 27, 120.f)   // Release
SP( 29, 130.f)   // E2 Decay
SP( 36, 12.f)    // LFO>Pitch
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP(109, 1)       // Arp On
SP(110, 9)       // Arp Mode = Pinky
SP(115, 72.f)    // Arp Gate
SP(116, 24.f)    // Arp Swing
SP(119, 2)       // Arp Vel Mode = Pattern
SPARP("fbfbfcfbfdfbfcfb", "05070507050705070507050705070507")
SP_END()

// LD Random Spark — Random mode with Chance under it: the note is drawn and the
// step may not fire at all, but the grid never slips. Macro 1: opens the filter.
SP_PRESET("LD Random Spark")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.5f)    // A Position
SP( 18, 3600.f)  // Cutoff
SP( 19, 0.4f)    // Resonance
SP( 22, 0.5f)    // Env2>Cutoff
SP( 25, 110.f)   // Decay
SP( 26, 0.1f)    // Sustain
SP( 27, 140.f)   // Release
SP( 29, 80.f)    // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.55f)   // M1 Amt
SP(109, 1)       // Arp On
SP(110, 6)       // Arp Mode = Random
SP(113, 2)       // Arp Octaves
SP(115, 44.f)    // Arp Gate
SP(119, 2)       // Arp Vel Mode = Pattern
SP(122, 68.f)    // Arp Chance
SPARP("fc0bfd0af90cfb0d", "05050007050500050705000505070005")
SP_END()

// LD Step Filter — the level row is a filter staircase, not a velocity map:
// source 17 walks the cutoff with the pattern. Macro 1: sets the filter floor.
SP_PRESET("LD Step Filter")
SP(  1, 0.3f)    // A Position
SP(  5, 4)       // A Unison
SP(  6, 16.f)    // A Detune
SP(  7, 0.7f)    // A Spread
SP( 18, 400.f)   // Cutoff
SP( 19, 0.6f)    // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 5.f)     // Drive
SP( 25, 400.f)   // Decay
SP( 26, 0.7f)    // Sustain
SP( 27, 150.f)   // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 17)      // M2 Src = Arp Step
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.6f)    // M2 Amt
SP(102, 2)       // M2 Curve = S-curve
SP(109, 1)       // Arp On
SP(110, 2)       // Arp Mode = Up-Down Inclusive
SP(115, 62.f)    // Arp Gate
SPARP("159dfb7326aeec84", "05050507050505030505050705050505")
SP_END()

// LD Tie Anthem — ties make it sing: one note held across seven steps, then the
// next, so a 1/16 grid plays a slow melody. Macro 1: opens the filter.
SP_PRESET("LD Tie Anthem")
SP(  1, 0.55f)   // A Position
SP(  5, 5)       // A Unison
SP(  6, 20.f)    // A Detune
SP(  7, 0.8f)    // A Spread
SP( 10, -12)     // B Coarse
SP( 12, 0.35f)   // B Level
SP( 18, 5200.f)  // Cutoff
SP( 19, 0.2f)    // Resonance
SP( 24, 6.f)     // Attack
SP( 26, 0.85f)   // Sustain
SP( 27, 260.f)   // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP(109, 1)       // Arp On
SP(115, 96.f)    // Arp Gate
SP(119, 1)       // Arp Vel Mode = Fixed
SP(120, 112)     // Arp Fixed Vel
SPARP("ffffffffffffffff", "05151515151515150515151507151515")
SP_END()

// ---------------------------------------------------------- Pad (1), bank 3

// PD Latch Drift — Hold latches the chord and Retrig 0 keeps the grid's place,
// so chords change under a running phrase. Macro 1: opens the filter.
SP_PRESET("PD Latch Drift")
SP(  1, 0.3f)    // A Position
SP(  5, 5)       // A Unison
SP(  6, 22.f)    // A Detune
SP(  7, 0.9f)    // A Spread
SP( 18, 2200.f)  // Cutoff
SP( 19, 0.25f)   // Resonance
SP( 24, 120.f)   // Attack
SP( 26, 1.f)     // Sustain
SP( 27, 900.f)   // Release
SP( 39, 14)      // Voices
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 17)      // M2 Src = Arp Step
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.45f)   // M2 Amt
SP(102, 2)       // M2 Curve = S-curve
SP(109, 1)       // Arp On
SP(110, 2)       // Arp Mode = Up-Down Inclusive
SP(112, 5)       // Arp Sync = 1/4
SP(113, 2)       // Arp Octaves
SP(115, 145.f)   // Arp Gate — over 100 %, so the steps overlap into a pad
SP(117, 1)       // Arp Hold
SP(118, 0)       // Arp Retrig — the grid keeps its place across chord changes
SP(121, 12)      // Arp Steps
SPARP("8acfedbacd98fbac", "05050705050305070505030507050505")
SP_END()

// --------------------------------------------------------- Keys (2), bank 3

// KY Bell Ladder — the octave column climbs and falls a two-octave ladder under
// the played chord, on a triplet grid. Macro 1: opens the filter.
SP_PRESET("KY Bell Ladder")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.35f)   // A Position
SP( 18, 4200.f)  // Cutoff
SP( 19, 0.2f)    // Resonance
SP( 22, 0.35f)   // Env2>Cutoff
SP( 25, 260.f)   // Decay
SP( 26, 0.2f)    // Sustain
SP( 27, 300.f)   // Release
SP( 29, 160.f)   // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.3f)    // M2 Amt
SP(109, 1)       // Arp On
SP(112, 9)       // Arp Sync = 1/8T
SP(115, 66.f)    // Arp Gate
SP(119, 2)       // Arp Vel Mode = Pattern
SPARP("fdb9fca8fdb9fca8", "05070907050305070907050305070503")
SP_END()

// KY Held Chime — ties hold half of a fourteen-step pattern, so the chime rings
// long over short instead of even. Macro 1: opens the filter.
SP_PRESET("KY Held Chime")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.2f)    // A Position
SP(  8, 2)       // B Table = Harmonic
SP(  9, 0.3f)    // B Position
SP( 12, 0.3f)    // B Level
SP( 18, 3800.f)  // Cutoff
SP( 19, 0.18f)   // Resonance
SP( 25, 700.f)   // Decay
SP( 26, 0.3f)    // Sustain
SP( 27, 420.f)   // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.45f)   // M1 Amt
SP(109, 1)       // Arp On
SP(110, 2)       // Arp Mode = Up-Down Inclusive
SP(115, 84.f)    // Arp Gate
SP(116, 18.f)    // Arp Swing
SP(119, 1)       // Arp Vel Mode = Fixed
SP(120, 88)      // Arp Fixed Vel
SP(121, 14)      // Arp Steps
SPARP("ffffffffffffffff", "05150515150515050515150515050505")
SP_END()

// -------------------------------------------------------- Pluck (4), bank 3

// PL Grid Rest — six rests in sixteen: the gaps are the groove, and a step that
// does not sound still advances the melody. Macro 1: opens the filter.
SP_PRESET("PL Grid Rest")
SP(  1, 0.4f)    // A Position
SP( 18, 900.f)   // Cutoff
SP( 19, 0.55f)   // Resonance
SP( 22, 0.7f)    // Env2>Cutoff
SP( 25, 90.f)    // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 70.f)    // Release
SP( 29, 70.f)    // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP(109, 1)       // Arp On
SP(110, 3)       // Arp Mode = Up-Down Exclusive
SP(115, 34.f)    // Arp Gate
SP(116, 42.f)    // Arp Swing
SP(119, 2)       // Arp Vel Mode = Pattern
SPARP("0fc0d0ba0c0df0b0", "00050500050005050005000505000500")
SP_END()

// PL Harp Climb — the octave column is the shape: sixteen steps walking from an
// octave below the chord to two above and back. Macro 1: opens the filter.
SP_PRESET("PL Harp Climb")
SP(  0, 2)       // A Table = Harmonic
SP(  1, 0.25f)   // A Position
SP( 18, 2800.f)  // Cutoff
SP( 19, 0.3f)    // Resonance
SP( 22, 0.5f)    // Env2>Cutoff
SP( 23, 0.5f)    // Keytrack
SP( 25, 140.f)   // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 200.f)   // Release
SP( 29, 90.f)    // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP(109, 1)       // Arp On
SP(115, 54.f)    // Arp Gate
SP(119, 2)       // Arp Vel Mode = Pattern
SPARP("9abcdeffedcba987", "03030505050707070909070705050303")
SP_END()

// PL Ratchet Tie — ties and rests together: a picked figure where some notes
// ring across a step and others drop out. Macro 1: opens the filter.
SP_PRESET("PL Ratchet Tie")
SP(  0, 5)       // A Table = Digital
SP(  1, 0.3f)    // A Position
SP( 18, 1800.f)  // Cutoff
SP( 19, 0.45f)   // Resonance
SP( 21, 4.f)     // Drive
SP( 22, 0.6f)    // Env2>Cutoff
SP( 25, 120.f)   // Decay
SP( 26, 0.05f)   // Sustain
SP( 27, 90.f)    // Release
SP( 29, 80.f)    // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP(109, 1)       // Arp On
SP(115, 46.f)    // Arp Gate
SP(119, 2)       // Arp Vel Mode = Pattern
SPARP("ff0cff0dfe0cff0e", "05150005151500050515000515150005")
SP_END()

// PL Step Morph — the level row walks the wavetables: source 17 drives A's
// position up while B's slides down. Macro 1: opens the filter.
SP_PRESET("PL Step Morph")
SP(  0, 7)       // A Table = Fold
SP(  1, 0.1f)    // A Position
SP(  8, 3)       // B Table = Formant
SP(  9, 0.85f)   // B Position
SP( 12, 0.4f)    // B Level
SP( 18, 3000.f)  // Cutoff
SP( 19, 0.3f)    // Resonance
SP( 25, 160.f)   // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 160.f)   // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 71, 17)      // M2 Src = Arp Step
SP( 72, 1)       // M2 Dst = A Pos
SP( 73, 0.7f)    // M2 Amt
SP( 74, 17)      // M3 Src = Arp Step
SP( 75, 2)       // M3 Dst = B Pos
SP( 76, -0.7f)   // M3 Amt
SP(103, 1)       // M3 Curve = Exp
SP(109, 1)       // Arp On
SP(115, 58.f)    // Arp Gate
SP(119, 2)       // Arp Vel Mode = Pattern
SPARP("013579bdfdb97531", "05050505070505050505050503050505")
SP_END()

// ----------------------------------------------------------- FX (1), bank 3

// FX Chance Rain — Random notes, a Chance of 42 % and half the steps resting:
// the pattern rains rather than runs. Macro 1: opens the band.
SP_PRESET("FX Chance Rain")
SP(  0, 4)       // A Table = Bell
SP(  1, 0.7f)    // A Position
SP( 16, 0.15f)   // Noise
SP( 18, 5000.f)  // Cutoff
SP( 19, 0.55f)   // Resonance
SP( 20, 1)       // Filter Type = BP12
SP( 25, 180.f)   // Decay
SP( 26, 0.f)     // Sustain
SP( 27, 400.f)   // Release
SP( 44, 0.5f)    // Noise Color
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.6f)    // M1 Amt
SP( 71, 17)      // M2 Src = Arp Step
SP( 72, 16)      // M2 Dst = Pan
SP( 73, 0.8f)    // M2 Amt
SP( 74, 13)      // M3 Src = Random
SP( 75, 7)       // M3 Dst = A Pitch
SP( 76, 0.05f)   // M3 Amt
SP(109, 1)       // Arp On
SP(110, 6)       // Arp Mode = Random
SP(113, 3)       // Arp Octaves
SP(114, 2)       // Arp Oct Mode = Alternate
SP(115, 70.f)    // Arp Gate
SP(119, 2)       // Arp Vel Mode = Pattern
SP(122, 42.f)    // Arp Chance
SPARP("d3f1a2e0c4b2f1d0", "05000700050900050003050009000500")
SP_END()

// ----------------------------------------------------- Sequence (4), bank 3

// SQ As Played Run — As Played mode is the point: the steps follow the order
// your fingers landed, not the pitch order. Macro 1: opens the filter.
SP_PRESET("SQ As Played Run")
SP(  1, 0.35f)   // A Position
SP(  5, 2)       // A Unison
SP(  6, 9.f)     // A Detune
SP( 18, 1500.f)  // Cutoff
SP( 19, 0.5f)    // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 5.f)     // Drive
SP( 22, 0.5f)    // Env2>Cutoff
SP( 25, 160.f)   // Decay
SP( 26, 0.3f)    // Sustain
SP( 27, 90.f)    // Release
SP( 29, 110.f)   // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 98, 1)       // Voice Mode = Mono
SP(109, 1)       // Arp On
SP(110, 5)       // Arp Mode = As Played
SP(115, 62.f)    // Arp Gate
SP(116, 58.f)    // Arp Swing
SP(118, 0)       // Arp Retrig — free-run, the grid owns the phrase
SP(119, 2)       // Arp Vel Mode = Pattern
SPARP("fcb9fdc8fbb9fdca", "05050305050705050305050705030505")
SP_END()

// SQ Bass Tie Grid — a tied bassline written into the grid: long roots, short
// answers, one leap up at the end of the bar. Macro 1: opens the filter.
SP_PRESET("SQ Bass Tie Grid")
SP(  1, 0.2f)    // A Position
SP( 17, 0.4f)    // Sub
SP( 18, 380.f)   // Cutoff
SP( 19, 0.62f)   // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 9.f)     // Drive
SP( 22, 0.55f)   // Env2>Cutoff
SP( 24, 0.4f)    // Attack
SP( 25, 200.f)   // Decay
SP( 26, 0.4f)    // Sustain
SP( 27, 60.f)    // Release
SP( 29, 130.f)   // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.55f)   // M1 Amt
SP( 71, 6)       // M2 Src = Velocity
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.3f)    // M2 Amt
SP( 98, 1)       // Voice Mode = Mono
SP(109, 1)       // Arp On
SP(115, 44.f)    // Arp Gate
SP(119, 2)       // Arp Vel Mode = Pattern
SPARP("fddafce9fddafcb7", "03151505031505050315151505150507")
SP_END()

// SQ Octave Stair — twelve steps stairing from two octaves down to two up and
// back; the octave column is the whole melody. Macro 1: opens the filter.
SP_PRESET("SQ Octave Stair")
SP(  0, 5)       // A Table = Digital
SP(  1, 0.3f)    // A Position
SP( 18, 1900.f)  // Cutoff
SP( 19, 0.48f)   // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 22, 0.45f)   // Env2>Cutoff
SP( 25, 150.f)   // Decay
SP( 26, 0.25f)   // Sustain
SP( 27, 80.f)    // Release
SP( 29, 100.f)   // E2 Decay
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.5f)    // M1 Amt
SP( 98, 1)       // Voice Mode = Mono
SP(109, 1)       // Arp On
SP(115, 56.f)    // Arp Gate
SP(116, 22.f)    // Arp Swing
SP(119, 2)       // Arp Vel Mode = Pattern
SP(121, 12)      // Arp Steps
SPARP("89abcdefedcb9876", "01030507090705030103050705050505")
SP_END()

// SQ Step Duet — one grid, two jobs: the level row is the velocity map and the
// filter walk at once, through source 17. Macro 1: sets the filter floor.
SP_PRESET("SQ Step Duet")
SP(  1, 0.15f)   // A Position
SP(  5, 3)       // A Unison
SP(  6, 11.f)    // A Detune
SP(  7, 0.55f)   // A Spread
SP( 18, 500.f)   // Cutoff
SP( 19, 0.58f)   // Resonance
SP( 20, 3)       // Filter Type = LP24
SP( 21, 6.f)     // Drive
SP( 25, 300.f)   // Decay
SP( 26, 0.55f)   // Sustain
SP( 27, 110.f)   // Release
SP( 68, 9)       // M1 Src = Macro 1
SP( 69, 11)      // M1 Dst = Cutoff
SP( 70, 0.4f)    // M1 Amt
SP( 71, 17)      // M2 Src = Arp Step
SP( 72, 11)      // M2 Dst = Cutoff
SP( 73, 0.65f)   // M2 Amt
SP( 74, 17)      // M3 Src = Arp Step
SP( 75, 12)      // M3 Dst = Resonance
SP( 76, 0.25f)   // M3 Amt
SP(102, 1)       // M2 Curve = Exp
SP(109, 1)       // Arp On
SP(110, 3)       // Arp Mode = Up-Down Exclusive
SP(115, 64.f)    // Arp Gate
SP(119, 2)       // Arp Vel Mode = Pattern
SPARP("fc84fc84b962b962", "05050705050503050507050505030505")
SP_END()
