import SwiftUI

/// The ROM as a map, filled in the order the recompiler really found the code.
/// Each cell is a slice of the ROM; arcs are real jumps and calls between them.
/// Code shows blue when found and turns green once the compiler has made it native.
final class RomExplorer {
    static let cols = 96, rows = 64
    struct Spark { var x, y, life: CGFloat; var guessed: Bool }
    struct Arc { var a, b: Int; var life: CGFloat }
    struct Tick { var at: Int; var address, bytes, native: String }

    private(set) var size = 1
    private var offsets: [Int32] = [], lengths: [UInt8] = []
    private var guessFrom = Int.max
    private var hirom = false
    private var edges: [(at: Int, a: Int32, b: Int32)] = []
    private var ticks: [Tick] = []
    private var known = Set<Int32>()

    private(set) var fill = [Float](repeating: 0, count: cols * rows)
    private(set) var guessed = [Bool](repeating: false, count: cols * rows)
    private(set) var heat = [Float](repeating: 0, count: cols * rows)
    private var pending = [Int32](repeating: 0, count: cols * rows)   // code in the cell not compiled yet
    private(set) var waiting = [Bool](repeating: false, count: cols * rows)
    private(set) var sparks: [Spark] = []
    private(set) var arcs: [Arc] = []
    private(set) var ticker: [Tick] = []
    private(set) var revealed = 0
    private(set) var compiledBytes = 0
    private var unbuilt = false
    private var total = 0, nextEdge = 0, nextTick = 0, rate = 1.0, carry = 0.0, last = Date()
    private var order: [Int] = []
    private var compiled = 0, compileTarget = 0, compileRate = 1.0, compileCarry = 0.0, nativeBytes = 0

    var active: Bool { revealed < order.count || compiled < compileTarget || !sparks.isEmpty || !arcs.isEmpty || heat.contains { $0 > 0.02 } }
    var hasData: Bool { !offsets.isEmpty }
    var progress: CGFloat { order.isEmpty ? 1 : CGFloat(compiled) / CGFloat(order.count) }
    var nativeShare: CGFloat { CGFloat(nativeBytes) / max(1, CGFloat(size) * 0.12) }
    var percentOfROM: Double { 100 * Double(compiledBytes) / Double(max(1, size)) }

    func isNative(_ cell: Int) -> Bool { pending[cell] == 0 }

    func cell(_ o: Int32) -> Int {
        min(Self.cols * Self.rows - 1, Int(Int64(o) * Int64(Self.cols * Self.rows) / Int64(max(1, size))))
    }

    /// Only what is new since the last map animates, so each round adds to the picture.
    func load(_ url: URL, instant: Bool = false, native: Bool = true) {
        if unbuilt {       // nothing on the map is compiled: start over rather than leave it blue
            unbuilt = false
            known = []
            fill = .init(repeating: 0, count: fill.count)
            guessed = .init(repeating: false, count: fill.count)
            pending = .init(repeating: 0, count: fill.count)
            compiledBytes = 0; nativeBytes = 0
        }
        guard let data = try? Data(contentsOf: url),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let off = j["offsets"] as? [Int], let len = j["lengths"] as? [Int] else { return }
        size = j["size"] as? Int ?? 1
        guessFrom = j["followed"] as? Int ?? Int.max
        hirom = j["hirom"] as? Bool ?? false
        offsets = off.map(Int32.init)
        lengths = len.map { UInt8(clamping: $0) }
        edges = (j["edges"] as? [[Int]] ?? []).map { ($0[0], Int32($0[1]), Int32($0[2])) }
        ticks = (j["ticker"] as? [[Any]] ?? []).compactMap {
            guard $0.count == 4, let at = $0[0] as? Int, let a = $0[1] as? String, let b = $0[2] as? String, let c = $0[3] as? String else { return nil }
            return Tick(at: at, address: a, bytes: b, native: c)
        }
        order = offsets.indices.filter { !known.contains(offsets[$0]) }
        total = known.count
        revealed = 0; nextEdge = 0; nextTick = 0
        compiled = 0; compileTarget = 0
        rate = max(400, Double(order.count) / 5.5)
        last = Date()
        if instant {
            while revealed < order.count { reveal(glow: false) }
            if native { while compiled < order.count { compile(glow: false) } } else { unbuilt = true }
            nextEdge = edges.count; nextTick = ticks.count
        }
    }

    /// The compiler is this far through the code in the last map.
    func compiling(_ fraction: Double) {
        compileTarget = max(compileTarget, min(order.count, Int(Double(order.count) * fraction)))
        compileRate = max(400, Double(order.count) / 5.5)
    }

    /// Code found by playing that has no native version yet stays blue.
    func setWaiting(_ lines: [Substring]) {
        waiting = .init(repeating: false, count: waiting.count)
        for l in lines {
            let f = l.split(separator: " ")
            var o: Int?
            if f[0] == "B", f.count > 2 { o = Int(f[2], radix: 16) }
            else if let pc = Int(f[0], radix: 16) { o = hirom ? pc & 0x3FFFFF : (pc >> 16 & 0x7F) << 15 | pc & 0x7FFF }
            if let o { waiting[cell(Int32(truncatingIfNeeded: o))] = true }
        }
    }

    func replay() {
        known = []
        fill = .init(repeating: 0, count: fill.count)
        guessed = .init(repeating: false, count: fill.count)
        pending = .init(repeating: 0, count: fill.count)
        compiledBytes = 0; nativeBytes = 0; total = 0; revealed = 0; nextEdge = 0; nextTick = 0
        order = Array(offsets.indices)
        rate = max(400, Double(order.count) / 7)
        compiled = 0; compileTarget = order.count; compileRate = rate
        last = Date()
    }

    private func reveal(glow: Bool) {
        let i = order[revealed]
        revealed += 1
        let o = offsets[i]
        known.insert(o)
        total += 1
        compiledBytes += Int(lengths[i])
        let c = cell(o)
        fill[c] = min(1, fill[c] + 0.04 + Float(lengths[i]) * 0.012)
        pending[c] += 1
        if i >= guessFrom { guessed[c] = true }
        if glow {
            heat[c] = 1
            while nextEdge < edges.count && edges[nextEdge].at <= i {
                if arcs.count < 70 { arcs.append(Arc(a: cell(edges[nextEdge].a), b: cell(edges[nextEdge].b), life: 1)) }
                nextEdge += 1
            }
            while nextTick < ticks.count && ticks[nextTick].at <= i {
                ticker.append(ticks[nextTick])
                if ticker.count > 5 { ticker.removeFirst() }
                nextTick += 1
            }
        }
    }

    private func compile(glow: Bool) {
        let i = order[compiled]
        compiled += 1
        nativeBytes += Int(lengths[i])
        let c = cell(offsets[i])
        pending[c] -= 1
        if glow && compiled % 11 == 0 && sparks.count < 260 {
            sparks.append(Spark(x: CGFloat(c % Self.cols), y: CGFloat(c / Self.cols), life: 1, guessed: i >= guessFrom))
        }
    }

    func step(_ now: Date) {
        let dt = min(0.1, now.timeIntervalSince(last))
        last = now
        carry += rate * dt
        while carry >= 1 && revealed < order.count { reveal(glow: true); carry -= 1 }
        if revealed >= order.count { carry = 0 }
        compileCarry += compileRate * dt
        while compileCarry >= 1 && compiled < min(compileTarget, revealed) { compile(glow: true); compileCarry -= 1 }
        if compiled >= min(compileTarget, revealed) { compileCarry = 0 }
        let fade = Float(pow(0.06, dt))
        for i in heat.indices where heat[i] > 0 { heat[i] = heat[i] * fade < 0.01 ? 0 : heat[i] * fade }
        for i in sparks.indices { sparks[i].life -= CGFloat(dt) * 1.5 }
        sparks.removeAll { $0.life <= 0 }
        for i in arcs.indices { arcs[i].life -= CGFloat(dt) * 1.6 }
        arcs.removeAll { $0.life <= 0 }
    }
}

struct ExplorerView: View {
    let explorer: RomExplorer
    let building: Bool
    let nativePercent: Double
    let facts: [RomFact]
    var program = false     // a disc's program, not a whole ROM
    @State private var kick = Date()

    let found = Color(red: 0.36, green: 0.72, blue: 1.0)
    let guessedColor = Color(red: 0.72, green: 0.5, blue: 1.0)
    let native = Color(red: 0.3, green: 0.95, blue: 0.62)

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(program ? "Program map" : "ROM map").font(.headline)
                Spacer()
                Legend(color: found, label: "Original code")
                if !program { Legend(color: guessedColor, label: "Guessed") }
                Legend(color: native, label: "Native")
                Legend(color: .white.opacity(0.1), label: "Data, or not reached")
                if !building && explorer.hasData {
                    Button { explorer.replay(); kick = Date() } label: { Image(systemName: "play.circle") }
                        .buttonStyle(.plain).help("Replay the exploration")
                }
            }
            RomFactsRow(facts: facts)
            TimelineView(.animation(minimumInterval: 1.0 / 60, paused: !building && !explorer.active)) { t in
                let _ = explorer.step(t.date)
                VStack(alignment: .leading, spacing: 6) {
                    Canvas { ctx, size in draw(ctx, size) }.frame(height: 310)
                    HStack {
                        Text(!explorer.hasData ? "Waiting for the first map" : program
                             ? String(format: "Code found fills %.1f%% of the program. The rest is its data, and the art, sound and levels are elsewhere on the disc.", explorer.percentOfROM)
                             : String(format: "Code found fills %.1f%% of the ROM. Most of the rest is art, sound and levels, which never need compiling.", explorer.percentOfROM))
                        Spacer()
                    }.font(.caption).foregroundStyle(.secondary).monospacedDigit()
                    if building || explorer.active { VStack(alignment: .leading, spacing: 1) {
                        ForEach(0..<5, id: \.self) { i in
                            let line = i < explorer.ticker.count ? explorer.ticker[i] : nil
                            HStack(spacing: 8) {
                                Text(line?.address ?? " ").foregroundStyle(found)
                                Text(line?.bytes ?? "").frame(width: 84, alignment: .leading).foregroundStyle(.secondary)
                                if line != nil { Image(systemName: "arrow.right").font(.system(size: 8)).foregroundStyle(native) }
                                Text(line?.native ?? "").foregroundStyle(native.opacity(0.85)).lineLimit(1)
                            }
                            .opacity(Double(i + 1) / 5)
                        }
                    }
                    .font(.system(size: 10, design: .monospaced))
                    .frame(height: 70, alignment: .top) }
                }
            }
            .id(kick)
        }
        .padding(14).background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
    }

    func draw(_ ctx: GraphicsContext, _ size: CGSize) {
        let barWidth: CGFloat = 54, gap: CGFloat = 26, ruler: CGFloat = 50
        let cols = RomExplorer.cols, rows = RomExplorer.rows
        let w = (size.width - barWidth - gap - ruler) / CGFloat(cols), h = size.height / CGFloat(rows)
        func centre(_ c: Int) -> CGPoint { CGPoint(x: ruler + (CGFloat(c % cols) + 0.5) * w, y: (CGFloat(c / cols) + 0.5) * h) }

        let mb = Double(explorer.size) / 1_048_576
        for i in 0..<4 {
            let label = Text(String(format: "%.2g MB", mb * Double(i) / 4)).font(.system(size: 9, design: .monospaced)).foregroundColor(.secondary)
            ctx.draw(label, at: CGPoint(x: ruler - 5, y: size.height * CGFloat(i) / 4 + 6), anchor: .trailing)
        }

        for r in 0..<rows {
            for c in 0..<cols {
                let i = r * cols + c
                let f = explorer.fill[i], heat = explorer.heat[i]
                let rect = CGRect(x: ruler + CGFloat(c) * w + 0.5, y: CGFloat(r) * h + 0.5, width: w - 1, height: h - 1)
                var colour = Color.white.opacity(0.045)
                if f > 0 {
                    let c = !explorer.isNative(i) ? (explorer.guessed[i] ? guessedColor : found) : native
                    colour = c.opacity(Double(0.25 + f * 0.65))
                }
                if explorer.waiting[i] { colour = found }
                ctx.fill(Path(rect), with: .color(colour))
                if heat > 0 { ctx.fill(Path(rect.insetBy(dx: -1, dy: -1)), with: .color(.white.opacity(Double(heat) * 0.85))) }
            }
        }
        for a in explorer.arcs {
            let p = centre(a.a), q = centre(a.b)
            var path = Path()
            path.move(to: p)
            let lift = min(70, abs(q.x - p.x) * 0.3 + abs(q.y - p.y) * 0.3 + 14)
            path.addQuadCurve(to: q, control: CGPoint(x: (p.x + q.x) / 2, y: min(p.y, q.y) - lift))
            ctx.stroke(path, with: .color(.white.opacity(Double(a.life) * 0.5)), lineWidth: 0.8)
        }

        // the library being built: it fills with what has been compiled
        let bar = CGRect(x: size.width - barWidth, y: 0, width: barWidth, height: size.height)
        ctx.fill(Path(roundedRect: bar, cornerRadius: 6), with: .color(.white.opacity(0.05)))
        let share = nativePercent > 0 ? CGFloat(nativePercent / 100) * explorer.progress : min(1, explorer.nativeShare)
        let level = CGRect(x: bar.minX + 3, y: bar.maxY - 3 - (bar.height - 6) * share, width: bar.width - 6, height: (bar.height - 6) * share)
        ctx.fill(Path(roundedRect: level, cornerRadius: 4), with: .linearGradient(Gradient(colors: [native, native.opacity(0.45)]),
                 startPoint: level.origin, endPoint: CGPoint(x: level.minX, y: level.maxY)))
        ctx.draw(Text(nativePercent > 0 ? String(format: "%.1f%%", nativePercent) : "native").font(.system(size: 9, weight: .semibold)).foregroundColor(.white.opacity(0.8)), at: CGPoint(x: bar.midX, y: 9))

        for s in explorer.sparks {
            let from = CGPoint(x: ruler + (s.x + 0.5) * w, y: (s.y + 0.5) * h)
            let to = CGPoint(x: bar.midX, y: level.minY + 4)
            let t = 1 - s.life, e = t * t * (3 - 2 * t)
            let at = CGPoint(x: from.x + (to.x - from.x) * e, y: from.y + (to.y - from.y) * e - sin(t * .pi) * 24)
            let dot = CGRect(x: at.x - 1.6, y: at.y - 1.6, width: 3.2, height: 3.2)
            ctx.fill(Path(ellipseIn: dot), with: .color((t > 0.6 ? native : s.guessed ? guessedColor : found).opacity(Double(min(1, s.life * 2)))))
        }
    }
}

struct Legend: View {
    let color: Color
    let label: String
    var body: some View {
        HStack(spacing: 4) {
            RoundedRectangle(cornerRadius: 2).fill(color).frame(width: 9, height: 9)
            Text(label).font(.caption2).foregroundStyle(.secondary)
        }
    }
}
