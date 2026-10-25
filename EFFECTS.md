# NightDriverStrip Effects

Most of these effects are Dave Plummer's and the NightDriverStrip
contributors' work. Many matrix effects are ports from
[Aurora](https://github.com/pixelmatix/aurora) (Jason Coon), Stefan Petrick's
noise sketches and the [SoulMate Lights](https://editor.soulmatelights.com/)
gallery, credited in each source file. This fork added the effects marked
*(fork)*.

## How effects are chosen

Each effect is a C++ class (`include/effects/strip/` for strips,
`include/effects/matrix/` for HUB75 matrices). `LoadEffectFactories()` in
`src/effects.cpp` registers instances of them in **effect sets**, and each
build environment picks a set with a `-DEFFECTS_<SET>` flag in
`platformio.ini`. The device rotates through the enabled effects in its set
every *effect interval* (set on the Effects tab; it can be "eternal"), and the
web UI's effect list can enable, disable, reorder, copy, rename and configure
them. The list is saved on the device; flashing firmware with a different
effect set resets it to the defaults.

Every effect has these settings in its ⚙ dialog:

| Setting | Meaning |
| - | - |
| Friendly name | The name shown in the list and, on matrices, the caption |
| Maximum effect time | Show this effect for less than the normal interval |
| Brightness override | 1-100 %, applied on top of the device brightness and nightly schedule while this effect runs |
| Frame rate override | 1-120 fps instead of the effect's own rate |

Effects can also be pinned to a single strip (the numbered buttons on each
row), which stops rotation until ▶ is pressed again.

## Effect sets

| Set | Used by | Contents |
| - | - | - |
| `DEMO` | This fork's XIAO ESP32-C5 and ESP32-S3 builds, `demo`, the M5Stick, Heltec and LilyGO demos | General strip effects; [listed below](#the-demo-set) |
| `FULLMATRIX` (`MESMERIZER`) | Mesmerizer builds, `spectrum2`, this fork's Waveshare matrix builds | 62 matrix effects; [listed below](#matrix-effects) |
| `MINIMAL` | `ledstrip` and most single-purpose builds | Status indicator and rainbow fill |
| `SIMPLE` | Basic strip projects | Medium Fire, bouncing balls, meteors, night twinkle, stars, palette |
| `SPECTRUM` | M5Stick spectrum analyzers, `helmet` | Spectrum analyzer variants, GhostWave, stars |
| `FAN`, `UMBRELLA`, `ATOMLIGHT`, `SPIRALLAMP`, `PLATECOVER`, `XMASTREES`, `INSULATORS`, `CUBE`, `CHIEFTAIN`, `LANTERN`, `LASERLINE`, `HELMET`, `HEXAGON`, `PDPWOPR`, `MAGICMIRROR`, `TTGO`, `WROVERKIT`, `IMPERIAL`, `APA102_DEMO`, `TRIMLIGHT` | Dave's individual projects | Effects tuned for that piece of hardware; see `src/effects.cpp` |

## The DEMO set

The effects the XIAO strip controllers in this fork run, in registration
order. Names are the defaults; each can be renamed on the device. On builds
with audio (`xiao_sense`), Beat Fireworks, RGB Music Blend Stars and an
audio-driven Rainbow Twinkle Stars come first.

| Effect | Class | What it shows |
| - | - | - |
| Beat Fireworks (audio) | `FireworksEffect` | Particle bursts launched on beats |
| RGB Music Blend Stars (audio) | `StarEffect<MusicStar>` | Red, green and blue stars spawned by the music, blurred together |
| Rainbow Twinkle Stars (audio) | `StarEffect<MusicStar>` | Rainbow stars spawned by the music |
| Medium Fire | `FireEffect` | Heat-diffusion fire, mirrored |
| Bouncing Balls (3 and 8 balls) | `BouncingBallEffect` | Balls bouncing under gravity with damping, mirrored from both ends |
| Color Meteors (4 and 2 meteors) | `MeteorEffect` | Colored meteors with fading tails bouncing between the ends |
| Red / Green / Blue / Rainbow Twinkle Stars | `GlobalColorStarEffect<QuietStar>` | Stars fading in and out; they take the remote's global color when one is set |
| NightTwinkle | `NightTwinkleEffect` | A night-sky base color with white stars igniting and fading |
| Twinkle | `TwinkleEffect` | Random colored LEDs lighting and fading |
| Palette Effect (two variants) | `PaletteEffect` | A rainbow palette scrolling along the strip |
| Warm White: 2700K, Cool White: 6500K | `WarmGlowEffect` | Plain white light at a color temperature (one neutral 4000 K white on SK6812 RGBW builds) |
| Fire *(fork)* | `LEDCentralFireEffect` | LED-Central's fire, see below |
| CandyCane *(fork)* | `CandyCaneEffect` | LED-Central's candy cane, see below |
| Halloween *(fork)* | `HalloweenEffect` | LED-Central's Halloween, see below |
| Length Test *(fork)* | `LengthTestEffect` | A ruler for measuring strips, see below |

### Effects ported from LED-Central *(fork)*

These three are ports of LED-Central's effects, so a controller shows the same
looks when LED-Central isn't driving it (`include/effects/strip/ledcentraleffects.h`).
Each channel is drawn as its own strip with its own length and LED density;
sizes are tuned in LEDs on a 60 LED/m strip and scaled. The per-LED math is
integer, since the ESP32-C5 has no FPU.

**Fire**: NightDriverStrip's fire automaton as LED-Central extended it, with a
slow "breathing" height and a simulation sized to the strip's real length.

| Setting | Default | Meaning |
| - | - | - |
| Cooling | 3 | How fast the flame cools; higher is shorter and calmer |
| Sparking | 100 | Chance out of 255 that a spark attempt ignites |
| Sparks | 3 | Spark attempts every 20 ms |
| Spark height | 4 | How far from the base a spark can start, in cells |
| Palette | red | Color family as it cools: red, green or blue |
| Reversed | off | Burn from the far end instead of LED 0 |
| Mirrored | off | Burn from the middle outward; with Reversed, from both ends inward |
| Minimum / Average / Maximum height | 0.2 / 0.7 / 1 | How low it breathes down to, where it mostly rests, how high it reaches (fractions of the strip) |
| Breathing period | 120 s | One slow rise and fall of the flame's height |

**CandyCane**: a candy cane spinning about its long axis, white with a wide
green and a narrow red helix stripe, so the stripes climb the strip as it
turns. Each channel views the cane from a different angle, so the stripes step
across neighboring strips. The spin follows NTP time, so canes on different
controllers turn together.

| Setting | Default | Meaning |
| - | - | - |
| Twists per second | 0.5 | Spin speed |
| Twist length | 40 | LEDs per full twist on a 60 LED/m strip |
| Green / Red stripe width | 30 % / 8 % | Share of each twist |
| View spread | 360° | Angle this controller's strips are spread over; 0 shows the same view on all |
| View offset | 0° | Added to every strip's view, to continue the pattern from one controller to the next |
| White / Green / Red color | | The cane's colors |
| Reversed | off | Climb toward LED 0 |

**Halloween**: pools of flickering jack-o'-lantern orange drifting along the
strip with black between them, pairs of creepy eyes that open, stare, blink
and fade, and lightning. The glow and eyes are seeded per channel; the
lightning is keyed to NTP time, so every controller strikes together.

| Setting | Default | Meaning |
| - | - | - |
| Glow color | (255, 90, 0) | Trimlight's "Jack-O-Lantern Glow" |
| Glow amount | 55 % | Share of the strip that glows |
| Glow size | 16 | Size of the pools, LEDs on a 60 LED/m strip |
| Glow speed | 0.4 | How fast the pools drift |
| Eye pairs | 1 | Pairs that can watch at once on each strip; 0 for none |
| Eye color / size / gap | (127, 255, 0) / 2 / 4 | |
| Lightning per minute | 3 | Average strikes; 0 turns lightning off |
| Lightning color | (216, 204, 255) | |

### Length Test *(fork)*

A ruler for finding each strip's exact LED count. On every channel the last
configured LED is full white; the LEDs before it carry ticks counted from LED
1 at the controller end: every 10th LED 50 % red, every other 5th LED 30 %
blue, the rest off.

1. Stop LED-Central (or its canvas) for this controller, so the local effect
   is shown.
2. Press ▶ on Length Test. It is a manual-only effect: rotation never picks
   it or leaves it, so it stays up until you choose another effect.
3. On the Settings tab, change the channel's strip length. While Length Test
   plays, every up/down arrow click is applied immediately and the white LED
   moves with it.
4. If the white LED stops short of the end, the strip is longer by the number
   of dark LEDs after it. If there is no white LED, the setting is too long;
   count back from the last red tick to the real end.

## Other strip effects

Used by the other effect sets. Most were written for specific builds of
Dave's: fans, umbrellas, lamps, Christmas trees, glass insulators.

| Class | Default names | What it shows |
| - | - | - |
| `RainbowFillEffect` | | A scrolling rainbow |
| `ColorFillEffect` | Solid White, Solid Red, Solid Amber, Off | One color, optionally on every Nth LED; follows the remote's global color |
| `StatusEffect` | | Every 10th LED shows system state: red no WiFi, purple OTA, green no clock yet, white ready |
| `ClassicFireEffect`, `FireEffect` | Calm Fire, Medium Fire | Fire2012-style fire automatons |
| `MusicalPaletteFire` | Musical Red / Blue / Green / Purple Fire | Palette fire with extra sparks on beats |
| `DoublePaletteEffect` | | Two palettes scrolling in opposite directions |
| `StarEffect<Star>`, `<BubblyStar>`, `<LongLifeSparkleStar>`, `<MusicStar>` | Sparkle Stars, Blooming Rainbow Stars, Neon Bars, Lava Stars, Music Stars | Particle stars with different ignite, hold and fade behavior |
| `SilonEffect` | | A Battlestar Galactica scanner |
| `ColorCycleEffect`, `FireFanEffect`, `FanBeatEffect` | | Color cycles, fire and beat effects laid out for LED fans |
| `PaletteReelEffect`, `TapeReelEffect` | | Spinning reel effects for fans and insulators |
| `ColorBeatOverRed`, `ColorBeatOnRedBkgnd`, `SimpleInsulatorBeatEffect2`, `SparklySpinningMusicEffect`, `InsulatorSpectrumEffect` | | Beat-driven particle effects for the insulator and cube builds |
| `MoltenGlassOnVioletBkgnd`, `NewMoltenGlassOnVioletBkgnd` | Molten Glass | Glowing beat-driven particles on a violet background |
| `VUMeterVerticalEffect` | | A vertical VU meter |
| `LanternEffect` | | A flickering candle lantern |
| `LaserLineEffect` | | Beat-launched laser lines |
| `OuterHexRingEffect` | | For the hexagon build |
| `PDPGridEffect`, `PDPCMXEffect` | | Front-panel lights for a PDP-11/34, including a Connection Machine CM-5 simulation |

## Matrix effects

The `FULLMATRIX` set (Mesmerizer and Waveshare HUB75 builds), as listed by
the device. Audio effects need a microphone build.

| Effect | Class | What it shows |
| - | - | - |
| Audiograph | `SpectrumBarEffect` | Spectrum bars mirrored into four quadrants (audio) |
| Spectrum, Spectrum 2, Spectrum++, USA, AudioWave | `SpectrumAnalyzerEffect` | 16-band spectrum analyzers with a VU meter, in different palettes and styles (audio) |
| GhostWave, WaveOut, PlasmaWave | `GhostWave` | A waveform blooming outward from the center (audio) |
| WaveIn | `WaveformEffect` | A scrolling waveform (audio) |
| Stars | `StarEffect<MusicStar>` | Music-spawned stars (audio) |
| Fire Log, Pacman, Colorball, Rings, Atomic, Bananaman, Tesseract, Nyancat, On Air | `PatternAnimatedGIF` | Animated GIFs embedded in flash |
| Clock | `PatternClock` | An analog clock |
| PongClock | `PatternPongClock` | Two paddles playing Pong, with the time as the score |
| Weather | `PatternWeather` | Current weather and forecast (needs an OpenWeather API key) |
| Stocks | `PatternStocks` | Stock quotes from a quote server |
| Subs | `PatternSubscribers` | A YouTube channel's subscriber count |
| AlienText | `PatternAlienText` | Random alien glyphs |
| Life | `PatternLife` | Conway's Game of Life, restarting when it loops |
| Maze | `PatternMaze` | A maze being generated |
| Munch | `PatternMunch` | The classic munching-squares pattern |
| Bounce | `PatternBounce` | Bouncing balls with trails |
| Cubes | `PatternCube` | Rotating 3D cubes |
| Circuit | `PatternCircuit` | Circuit-board traces |
| MRI | `PatternMandala` | A shifting, symmetric mandala |
| Pulsars | `PatternPulsar` | Expanding rings |
| Rose, Pinwheel, Sunburst | `PatternRose`, `PatternPinwheel`, `PatternSunburst` | Geometric patterns from Aurora |
| Swirl, Wave, Serendipity | `PatternSwirl`, `PatternWave`, `PatternSerendipity` | Flowing color patterns from Aurora |
| Fireplace | `PatternSMFire2021` | Perlin-noise fire |
| RadialFire | `PatternSMRadialFire` | Fire radiating from the center |
| RadialWave | `PatternSMRadialWave` | Concentric color waves |
| Smoke | `PatternSMSmoke` | Drifting smoke |
| Shikon | `PatternSMNoise` | Palette noise cycling through 17 variations, like a surreal topographic map |
| Liquidflow | `PatternSMFlowFields` | Particles following a flow field |
| Powder | `PatternSMBlurringColors` | Blurring colored powder |
| MetaBalls | `PatternSMMetaBalls` | Merging blobs |
| Supernova | `PatternSMSupernova` | Exploding particles |
| Gamma | `PatternSMGamma` | An abstract animated pattern (SoulMate "Q24") |
| Machine | `PatternSMWalkingMachine` | SoulMate's "Walking Machine" |
| Hypnosis | `PatternSMHypnosis` | Hypnotic spirals |
| Star Deep | `PatternSMStarDeep` | Drifting multi-point stars |
| Crystallize | `PatternSM2DDPR` | Symmetric patterns growing from the center |
| Lines, Circles | `PatternSMPicasso3in1` | Picasso-style moving lines and circles |
| Color Rain | `PatternSMAmberRain` | Falling colored rain |
| Diffusion | `PatternSMStrobeDiffusion` | Strobing diffusion |
| Colorspin | `PatternSMRainbowTunnel` | A rainbow tunnel |
| Spiro | `PatternSMSpiroPulse` | Pulsing spirograph curves |
| Twister | `PatternSMTwister` | A twisting column |
| Tannenbaum | `PatternSMHolidayLights` | Holiday lights on a tree |

The `PatternSM*` effects come from the SoulMate Lights gallery; the
`Pattern*` geometric and noise effects mostly from Aurora.
