import SwiftUI

/// The road from a ROM to a packaged app, and why each stop on it exists.
struct Stage: Identifiable {
    let id: Int
    let name: String
    let icon: String
    let color: Color
    let title: String
    let what: String
    let why: String
}

let stages: [Stage] = [
    Stage(id: 0, name: "ROM", icon: "memorychip", color: .gray,
          title: "Start with the cartridge",
          what: "Genexia works out which console the ROM is for and takes a fingerprint of every byte. That fingerprint names the game from here on.",
          why: "Translated code only fits the exact ROM it came from. Another revision of the same game moves everything, so the fingerprint keeps each game with its own code."),
    Stage(id: 1, name: "Find code", icon: "point.topleft.down.curvedto.point.bottomright.up", color: accent,
          title: "Find out what is code",
          what: "The game starts in the interpreter, which runs one instruction at a time. From there, every jump and branch is followed to reach more code.",
          why: "A ROM has no labels. Code, graphics and music sit side by side as plain bytes, and translating art as if it were code gives you nonsense. N64 games make it harder: they copy code into memory as they go, so it can only be seen by watching them run."),
    Stage(id: 2, name: "Translate", icon: "cpu", color: .purple,
          title: "Turn console code into native code",
          what: "Each stretch of code becomes a C function that does exactly what those instructions did. A normal compiler then turns the C into native code for this Mac.",
          why: "This all happens ahead of time, never during play. C lets a real optimizing compiler do the hard work, and the same C can target a modern Mac or an iMac G4."),
    Stage(id: 3, name: "Bots play", icon: "gamecontroller", color: .orange,
          title: "Let bots play to reach more",
          what: "Several bots play at once, pressing buttons fast, while the recorder notes any code that has no translation yet. That code is compiled and the bots go again.",
          why: "Some code can't be found by reading: a game can compute where to jump only while it runs. The only sure way to reach those places is to go there. When you play with recording on, you do the same job."),
    Stage(id: 4, name: "Measure", icon: "gauge.with.needle", color: .cyan,
          title: "Measure what really runs native",
          what: "Genexia counts how much of the game's CPU time ran in native code and how much fell back to the interpreter.",
          why: "How much of the ROM was translated says little, since games spend most of their time in a small share of their code. CPU time is the number you actually feel."),
    Stage(id: 5, name: "Verify", icon: "checkmark.seal", color: .green,
          title: "Prove it behaves the same",
          what: "The native code is checked against the interpreter. SNES blocks are each replayed and compared. N64 runs race the whole game on identical inputs, and all of memory must end up identical.",
          why: "Fast but subtly wrong is worse than slow. One wrong flag might only show up as a crash an hour into a game, so nothing is called finished without being compared."),
    Stage(id: 6, name: "Library", icon: "books.vertical", color: .pink,
          title: "Keep each game in its own library",
          what: "The finished native code is linked into one file for that game. Genexia finds it by fingerprint and loads it when the game starts.",
          why: "With the games kept out of the emulator, a new ROM never means rebuilding Genexia. Growing a game replaces one file and touches nothing else."),
    Stage(id: 7, name: "App", icon: "shippingbox", color: accent,
          title: "Package it as a real app",
          what: "The game's library, its ROM, the emulator and its options go into one Mac app with its own icon. It starts straight in the game, with nothing else installed.",
          why: "This is the goal: a ROM goes in, a double-clickable game comes out. The ROM and emulator are still inside for a reason, which the diagram below shows."),
]

struct HowItWorks: View {
    @State private var current = 0
    @State private var autoplay = true
    private let timer = Timer.publish(every: 7, on: .main, in: .common).autoconnect()

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 22) {
                VStack(alignment: .leading, spacing: 6) {
                    Text("How It Works").font(.system(size: 28, weight: .bold, design: .rounded))
                    Text("A ROM goes in. A native app comes out. Everything in between has a reason.")
                        .font(.title3).foregroundStyle(.secondary)
                }
                Road(current: $current, autoplay: $autoplay)
                StageCard(stage: stages[current])
                GrowLoop()
                InsideTheApp()
                ModernTake()
                Compare()
                YourGames()
            }
            .padding(.horizontal, 28).padding(.top, 34).padding(.bottom, 24)
        }
        .onReceive(timer) { _ in
            if autoplay { withAnimation(.easeInOut(duration: 0.35)) { current = (current + 1) % stages.count } }
        }
    }
}

/// Every stop in a line, with a spark travelling towards the current one.
struct Road: View {
    @Binding var current: Int
    @Binding var autoplay: Bool
    var body: some View {
        HStack(spacing: 0) {
            ForEach(stages) { s in
                Button {
                    autoplay = false
                    withAnimation(.easeInOut(duration: 0.3)) { current = s.id }
                } label: {
                    VStack(spacing: 7) {
                        ZStack {
                            Circle().fill(s.id == current ? s.color : s.id < current ? s.color.opacity(0.28) : Color.white.opacity(0.06))
                                .frame(width: 42, height: 42)
                            Image(systemName: s.icon).font(.system(size: 15, weight: .semibold))
                                .foregroundStyle(s.id == current ? .white : s.id < current ? s.color : .secondary)
                        }
                        Text(s.name).font(.caption2.weight(s.id == current ? .semibold : .regular))
                            .foregroundStyle(s.id == current ? .primary : .secondary).lineLimit(1).fixedSize()
                    }
                    .frame(maxWidth: .infinity).contentShape(Rectangle())
                }
                .buttonStyle(.plain).help(s.title)
                if s.id < stages.count - 1 {
                    Rectangle().fill(.white.opacity(s.id < current ? 0.4 : 0.1)).frame(height: 2).offset(y: -10).frame(maxWidth: 16)
                }
            }
        }
    }
}

struct StageCard: View {
    let stage: Stage
    var body: some View {
        HStack(alignment: .top, spacing: 20) {
            StageArt(stage: stage.id, color: stage.color)
                .frame(width: 270, height: 200)
                .background(Color.black.opacity(0.28), in: RoundedRectangle(cornerRadius: 12))
                .clipShape(RoundedRectangle(cornerRadius: 12))
            VStack(alignment: .leading, spacing: 10) {
                Text(stage.title).font(.system(size: 19, weight: .semibold, design: .rounded))
                Text(stage.what).font(.callout).fixedSize(horizontal: false, vertical: true)
                VStack(alignment: .leading, spacing: 4) {
                    Text("WHY").font(.system(size: 10, weight: .bold, design: .rounded)).foregroundStyle(stage.color)
                    Text(stage.why).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(16).frame(minHeight: 232, alignment: .top)
        .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 14))
        .id(stage.id)
        .transition(.opacity)
    }
}

/// A small moving picture of each stage.
struct StageArt: View {
    let stage: Int
    let color: Color
    @State private var start = Date()

    var body: some View {
        TimelineView(.animation) { tl in
            let t = tl.date.timeIntervalSince(start)
            Group {
                switch stage {
                case 2: Translate(t: t)
                case 4: Measure(t: t)
                case 5: Race(t: t)
                default: Canvas { ctx, size in draw(ctx, size, t) }
                }
            }
        }
        .onAppear { start = Date() }
    }

    func draw(_ context: GraphicsContext, _ size: CGSize, _ t: Double) {
        var ctx = context
        let cols = 24, rows = 16
        let w = size.width - 24, h = size.height - 24
        let cw = w / Double(cols), ch = h / Double(rows)
        func cell(_ i: Int) -> CGRect {
            CGRect(x: 12 + Double(i % cols) * cw, y: 12 + Double(i / cols) * ch, width: cw - 2, height: ch - 2)
        }
        // the same scatter of "really code" every time
        func isCode(_ i: Int) -> Bool { ((i &* 2654435761) >> 7) & 3 == 0 || (i / 7) % 5 == 1 }
        switch stage {
        case 0:   // unlabelled bytes, read by a passing line
            let line = Int(t * 9) % rows
            for i in 0..<(cols * rows) {
                let lit = i / cols == line
                let v = Double((i &* 2654435761) >> 11 & 7) / 24 + 0.06
                ctx.fill(Path(roundedRect: cell(i), cornerRadius: 1.5), with: .color(.white.opacity(lit ? 0.5 : v)))
            }
        case 1:   // a path of jumps lighting up the code among the data
            let code = (0..<(cols * rows)).filter(isCode)
            let found = min(code.count, Int(t * 14) % (code.count + 30))
            for i in 0..<(cols * rows) {
                ctx.fill(Path(roundedRect: cell(i), cornerRadius: 1.5), with: .color(.white.opacity(0.07)))
            }
            var from: CGPoint?
            for n in 0..<found {
                let c = code[(n &* 37) % code.count]
                ctx.fill(Path(roundedRect: cell(c), cornerRadius: 1.5), with: .color(color.opacity(0.85)))
                let p = CGPoint(x: cell(c).midX, y: cell(c).midY)
                if n >= found - 7, let f = from {
                    var path = Path()
                    path.move(to: f)
                    path.addQuadCurve(to: p, control: CGPoint(x: (f.x + p.x) / 2, y: min(f.y, p.y) - 18))
                    ctx.stroke(path, with: .color(.white.opacity(0.14 * Double(n - found + 8))), lineWidth: 1)
                }
                from = p
            }
        case 3:   // rounds of bots: each round finds more than the known share
            let code = (0..<(cols * rows)).filter(isCode)
            let phase = t.truncatingRemainder(dividingBy: 8) / 8
            let round = Int(phase * 4)
            let share = [0.35, 0.65, 0.85, 0.97][round]
            for i in 0..<(cols * rows) {
                ctx.fill(Path(roundedRect: cell(i), cornerRadius: 1.5), with: .color(.white.opacity(0.06)))
            }
            for (n, c) in code.enumerated() where Double((n &* 37) % code.count) / Double(code.count) < share {
                ctx.fill(Path(roundedRect: cell(c), cornerRadius: 1.5), with: .color(color.opacity(0.8)))
            }
            for b in 0..<4 {
                let i = Int(t * 6 + Double(b) * 40) &* (b + 3) % code.count
                ctx.fill(Path(ellipseIn: cell(code[i]).insetBy(dx: -3, dy: -3)), with: .color(.white))
            }
            ctx.draw(Text("round \(round + 1)").font(.caption.weight(.semibold)).foregroundColor(.white), at: CGPoint(x: size.width - 40, y: size.height - 14))
        case 6:   // a shelf of games, one file each
            let names = ["A1F3C07D", "7C20D9E4", "E5B8301A", "3D94F62B"]
            for (i, n) in names.enumerated() {
                let drop = min(1, max(0, (t - Double(i) * 0.7) / 0.5))
                let eased = 1 - (1 - drop) * (1 - drop)
                let r = CGRect(x: 40, y: 22 + Double(i) * 41 - (1 - eased) * 30, width: size.width - 80, height: 34)
                ctx.opacity = eased
                ctx.fill(Path(roundedRect: r, cornerRadius: 7), with: .color(color.opacity(0.28)))
                ctx.draw(Text(n + "  game").font(.system(size: 12, design: .monospaced)).foregroundColor(.white), at: CGPoint(x: r.midX, y: r.midY))
            }
        default:  // the pieces drop into one app
            let parts: [(String, Color)] = [("native game", .blue), ("emulator", .purple), ("ROM", .gray), ("options", .orange)]
            let box = CGRect(x: 60, y: 30, width: size.width - 120, height: size.height - 54)
            ctx.opacity = 1
            for (i, p) in parts.enumerated() {
                let drop = min(1, max(0, (t.truncatingRemainder(dividingBy: 7) - Double(i) * 0.6) / 0.6))
                let r = CGRect(x: box.minX + 12, y: box.minY + 10 + Double(i) * 30 - (1 - drop) * 50, width: box.width - 24, height: 25)
                ctx.opacity = drop
                ctx.fill(Path(roundedRect: r, cornerRadius: 5), with: .color(p.1.opacity(0.5)))
                ctx.draw(Text(p.0).font(.caption).foregroundColor(.white), at: CGPoint(x: r.midX, y: r.midY))
            }
            ctx.opacity = 1
            ctx.stroke(Path(roundedRect: box, cornerRadius: 12), with: .color(color), lineWidth: 2)
            ctx.draw(Text("Game.app").font(.caption.weight(.semibold)).foregroundColor(.white), at: CGPoint(x: box.midX, y: size.height - 11))
        }
    }
}

/// One instruction, three languages. An illustration, not any game's code.
struct Translate: View {
    let t: Double
    var body: some View {
        let shown = Int(t.truncatingRemainder(dividingBy: 6) / 1.3)
        VStack(alignment: .leading, spacing: 9) {
            row("CONSOLE", "ADD  r1, r2, r3", .orange, on: true)
            Image(systemName: "arrow.down").font(.caption).foregroundStyle(.secondary).opacity(shown >= 1 ? 1 : 0.2).padding(.leading, 10)
            row("C", "r[1] = r[2] + r[3];", .purple, on: shown >= 1)
            Image(systemName: "arrow.down").font(.caption).foregroundStyle(.secondary).opacity(shown >= 2 ? 1 : 0.2).padding(.leading, 10)
            row("THIS MAC", "add  w8, w9, w10", accent, on: shown >= 2)
        }
        .padding(14).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
    }
    func row(_ label: String, _ code: String, _ c: Color, on: Bool) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(label).font(.system(size: 9, weight: .bold, design: .rounded)).foregroundStyle(c)
            Text(code).font(.system(size: 14, design: .monospaced))
        }
        .opacity(on ? 1 : 0.15)
    }
}

struct Measure: View {
    let t: Double
    var body: some View {
        let p = min(1, t.truncatingRemainder(dividingBy: 7) / 3.5)
        VStack(alignment: .leading, spacing: 14) {
            bar("Share of the ROM translated", 0.2 * p, .gray)
            bar("Share of CPU time running native", 0.96 * p, accent)
            Text("A little code does most of the work.").font(.caption).foregroundStyle(.secondary)
        }
        .padding(16).frame(maxWidth: .infinity, maxHeight: .infinity)
    }
    func bar(_ label: String, _ v: Double, _ c: Color) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(label).font(.caption)
            GeometryReader { g in
                ZStack(alignment: .leading) {
                    Capsule().fill(.white.opacity(0.08))
                    Capsule().fill(c).frame(width: g.size.width * v)
                }
            }.frame(height: 10)
        }
    }
}

struct Race: View {
    let t: Double
    var body: some View {
        let p = min(1, t.truncatingRemainder(dividingBy: 7) / 4)
        VStack(alignment: .leading, spacing: 13) {
            track("Native code", p, accent)
            track("Interpreter", p, .orange)
            HStack(spacing: 6) {
                Image(systemName: p >= 1 ? "checkmark.seal.fill" : "equal.circle").foregroundStyle(p >= 1 ? .green : .secondary)
                Text(p >= 1 ? "Same inputs, same result" : "Same inputs…").font(.callout.weight(.medium))
            }
        }
        .padding(16).frame(maxWidth: .infinity, maxHeight: .infinity)
    }
    func track(_ label: String, _ v: Double, _ c: Color) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(label).font(.caption)
            GeometryReader { g in
                ZStack(alignment: .leading) {
                    Capsule().fill(.white.opacity(0.08))
                    Capsule().fill(c).frame(width: g.size.width * v)
                }
            }.frame(height: 9)
        }
    }
}

/// The loop that keeps a game growing after it's built.
struct GrowLoop: View {
    let steps: [(String, String)] = [
        ("Play", "gamecontroller"), ("New code found", "record.circle"), ("Waiting in the app", "sparkles"), ("Compile", "cpu"), ("More runs native", "bolt.fill"),
    ]
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("It keeps growing").font(.headline)
            HStack(spacing: 4) {
                ForEach(Array(steps.enumerated()), id: \.offset) { i, s in
                    VStack(spacing: 6) {
                        Image(systemName: s.1).font(.system(size: 17)).foregroundStyle(i == 1 ? .red : accent)
                        Text(s.0).font(.caption).multilineTextAlignment(.center).foregroundStyle(.secondary)
                    }.frame(maxWidth: .infinity)
                    Image(systemName: i < steps.count - 1 ? "arrow.right" : "arrow.uturn.backward").font(.caption).foregroundStyle(.tertiary)
                }
            }
            Text("Code without a translation just runs in the interpreter, so a game works from the first moment. With recording on, Genexia notes that code, and the next compile makes it native. There is no compiling during play.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .padding(14).background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
    }
}

/// What "native" covers, and what still has to be emulated.
struct InsideTheApp: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Inside the finished app").font(.headline)
            HStack(alignment: .top, spacing: 10) {
                part("Native game code", "cpu", accent, "What the console's main processor ran, now running directly on your Mac.")
                part("Interpreter", "tortoise", .orange, "The safety net for anything not translated yet, so the game never stops.")
                part("Chip emulators", "waveform", .purple, "Graphics and sound were separate chips. Your Mac has nothing like them, so they stay emulated.")
                part("The ROM", "memorychip", .gray, "The game's art, music and levels. They are data the game reads as it runs, so they stay too.")
            }
            .fixedSize(horizontal: false, vertical: true)
        }
        .padding(14).background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
    }
    func part(_ title: String, _ icon: String, _ c: Color, _ text: String) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            Image(systemName: icon).font(.system(size: 18)).foregroundStyle(c)
            Text(title).font(.system(size: 13, weight: .semibold))
            Text(text).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .padding(11).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(c.opacity(0.1), in: RoundedRectangle(cornerRadius: 10))
    }
}

/// Whose games these are, and what this is for.
struct YourGames: View {
    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: "heart.text.square").font(.system(size: 22)).foregroundStyle(accent)
            VStack(alignment: .leading, spacing: 6) {
                Text("Your games, your ROMs").font(.headline)
                Text("Genexia doesn't provide ROMs. You should own the real game and supply the ROM yourself.")
                    .font(.callout).fixedSize(horizontal: false, vertical: true)
                Text("The idea is to keep our games alive for new generations and run them on other devices, for fun, as an experiment and for personal enjoyment. A packaged app contains your ROM, so keep it for yourself.")
                    .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(14).frame(maxWidth: .infinity, alignment: .leading)
        .background(accent.opacity(0.08), in: RoundedRectangle(cornerRadius: 12))
    }
}

/// Why this road and not the two obvious ones.
/// What a game gains on the way, and which part of Genexia gives it.
struct ModernTake: View {
    let gains: [(icon: String, title: String, from: String, to: String, text: String)] = [
        ("hare", "Speed", "Emulated", "Native", "Super Mario 64 reaches about 3.7x full speed, and the time saved pays for everything else here."),
        ("square.resize.up", "Resolution", "320 x 240", "1280 x 960", "N64 3D is really drawn at up to 4x, with 32-bit colour in place of dithering."),
        ("rectangle.expand.vertical", "Widescreen", "4:3", "16:9", "More of the scene, not a stretched picture. Super Mario Kart draws real extra track."),
        ("circle.hexagongrid", "Anti-aliasing", "Jagged", "Smooth", "N64 edges are drawn at twice the size and averaged down."),
        ("square.grid.3x3", "Pixel art", "Blocky", "Scale4x", "Genesis and SNES corners are rounded without inventing colours."),
        ("desktopcomputer", "More machines", "One console", "Many Macs", "Apple silicon, Intel and PowerPC under Mac OS 9, each with code built for it.")
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                Text("A modern take on the original").font(.headline)
                Text("Native code brings the speed. Genexia's rebuilt video and sound chips bring the rest, so the same game can look better and run on machines it never knew. Options vary by console.")
                    .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 10), count: 3), spacing: 10) {
                ForEach(gains, id: \.title) { g in
                    VStack(alignment: .leading, spacing: 7) {
                        Label(g.title, systemImage: g.icon).font(.system(size: 13, weight: .semibold)).foregroundStyle(accent)
                        HStack(spacing: 5) {
                            Text(g.from).foregroundStyle(.secondary)
                            Image(systemName: "arrow.right").font(.system(size: 9))
                            Text(g.to).foregroundStyle(.green)
                        }.font(.system(size: 12, weight: .semibold, design: .rounded)).lineLimit(1).minimumScaleFactor(0.8)
                        Text(g.text).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                        Spacer(minLength: 0)
                    }
                    .padding(11).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                    .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 10))
                }
            }
        }
        .padding(14).background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
    }
}

struct Compare: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Why not something simpler?").font(.headline)
            line("Just emulate it", "Works for everything, but translating each instruction as it runs is slow. That cost is what rules out older machines.")
            line("Translate the whole ROM at once", "It can't be done reliably. Without running the game, nothing can tell code from data or see where computed jumps land.")
            line("Compile while playing", "That's a JIT, and it spends time and memory during play. Doing the work ahead of time means the game itself only runs finished code.")
        }
        .padding(14).background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
    }
    func line(_ title: String, _ text: String) -> some View {
        HStack(alignment: .top, spacing: 12) {
            Text(title).font(.system(size: 13, weight: .semibold)).frame(width: 190, alignment: .leading)
            Text(text).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
    }
}
