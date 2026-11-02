import Foundation
import SwiftUI

/// One game in the library, from its info.json, plus the state of a running build.
struct Game: Identifiable, Equatable {
    var id: String
    var title: String
    var console: String
    var romPath: String
    var status: String = "ready"
    var instructions: Int = 0
    var addresses: Int = 0
    var nativePercent: Double = 0
    var mode7Percent: Double = 0
    var followed: Int = 0
    var guessed: Int = 0
    var libraryBytes: Int = 0
    var os9Bytes: Int = 0
    var options = GameOptions()
    var waiting = 0            // addresses found by playing, not compiled yet
    var checkedBlocks: Int = 0
    var mismatches: Int = 0
    var os9Library: String? = nil
    var problem: String? = nil

    // a build in progress
    var building = false
    var queued = false
    var step = ""
    var detail = ""
    var done: Set<String> = []
    var log: [String] = []
}

let buildSteps: [(id: String, title: String, icon: String)] = [
    ("inspect", "Inspect", "magnifyingglass"),
    ("discover", "Find code", "point.topleft.down.curvedto.point.bottomright.up"),
    ("compile", "Compile", "cpu"),
    ("play", "Bots play", "gamecontroller"),
    ("measure", "Measure", "gauge.with.needle"),
    ("verify", "Verify", "checkmark.seal"),
    ("os9", "iMac library", "desktopcomputer"),
]

/// What Genexia applies when this game starts, kept in options.txt beside its library.
struct GameOptions: Equatable {
    var picture = 0     // 0 square pixels, 1 4:3 TV, 2 16:9, 3 ultrawide
    var filter = 0      // sharp, smooth, scanlines, CRT
    var sprites = 1     // 1 removes the hardware's per-line limit
    var runahead = 0
    var speed = 100
    var record = 1

    init() {}
    init(file: URL) {
        guard let t = try? String(contentsOf: file, encoding: .utf8) else { return }
        for line in t.split(separator: "\n") {
            let p = line.split(separator: " ")
            guard p.count == 2, let v = Int(p[1]) else { continue }
            switch p[0] {
            case "picture": picture = v
            case "filter": filter = v
            case "sprites": sprites = v
            case "runahead": runahead = v
            case "speed": speed = v
            case "record": record = v
            default: break
            }
        }
    }

    func save(to file: URL) {
        let t = "picture \(picture)\nfilter \(filter)\nsprites \(sprites)\nrunahead \(runahead)\nspeed \(speed)\nrecord \(record)\n"
        try? t.write(to: file, atomically: true, encoding: .utf8)
    }
}

@MainActor
final class Store: ObservableObject {
    @Published var games: [Game] = []
    @Published var selection: String?
    @Published var banner: String?
    @Published var packaging: String?
    @AppStorage("imacHost") var imacHost = "192.168.1.38"

    private var queue: [String] = []
    private var running = false

    let repo = Bundle.main.object(forInfoDictionaryKey: "GenexiaRepo") as? String
        ?? FileManager.default.currentDirectoryPath
    var gamesDir: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/Genexia/games")
    }

    init() {
        reload()
        Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.countWaiting() }
        }
    }

    private func addresses(_ url: URL) -> Set<Substring> {
        guard let t = try? String(contentsOf: url, encoding: .utf8) else { return [] }
        return Set(t.split(separator: "\n").filter { $0.count == 8 })
    }

    /// Genexia adds to recorded.txt during play, so this keeps up while a game runs.
    func countWaiting() {
        for i in games.indices where !games[i].building && !games[i].id.hasPrefix("drop-") {
            let d = gamesDir.appendingPathComponent(games[i].id)
            let played = addresses(d.appendingPathComponent("recorded.txt")).union(addresses(d.appendingPathComponent("imac-recorded.txt")))
            let n = played.isEmpty ? 0 : played.subtracting(addresses(d.appendingPathComponent("coverage.txt"))).count
            if games[i].waiting != n { games[i].waiting = n }
        }
    }

    func checkIMac(_ game: Game) {
        let name = String(game.title.prefix(26)).replacingOccurrences(of: ":", with: "_") + ".miss"
        var lines: [String] = []
        banner = "Asking the iMac what was recorded…"
        run(["python3", repo + "/tools/os9_share.py", imacHost, "get", name], onLine: { lines.append($0) }) { [weak self] code in
            guard let self else { return }
            guard code == 0 else { self.banner = "No recording from the iMac for \(game.title)."; return }
            let file = self.gamesDir.appendingPathComponent(game.id).appendingPathComponent("imac-recorded.txt")
            try? (lines.joined(separator: "\n") + "\n").write(to: file, atomically: true, encoding: .utf8)
            self.countWaiting()
            self.banner = "iMac recording checked."
        }
    }

    func reload() {
        let dirs = (try? FileManager.default.contentsOfDirectory(at: gamesDir, includingPropertiesForKeys: nil)) ?? []
        var found: [Game] = []
        for d in dirs {
            guard let data = try? Data(contentsOf: d.appendingPathComponent("info.json")),
                  let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { continue }
            var g = Game(id: d.lastPathComponent, title: j["title"] as? String ?? d.lastPathComponent,
                         console: j["console"] as? String ?? "", romPath: j["rom"] as? String ?? "")
            g.status = j["status"] as? String ?? "ready"
            g.instructions = j["instructions"] as? Int ?? 0
            g.addresses = j["addresses"] as? Int ?? 0
            g.nativePercent = (j["native_percent"] as? NSNumber)?.doubleValue ?? 0
            g.mode7Percent = (j["mode7_percent"] as? NSNumber)?.doubleValue ?? 0
            g.followed = j["followed"] as? Int ?? 0
            g.guessed = j["guessed"] as? Int ?? 0
            g.libraryBytes = j["library_bytes"] as? Int ?? 0
            g.os9Bytes = j["os9_bytes"] as? Int ?? 0
            g.options = GameOptions(file: d.appendingPathComponent("options.txt"))
            g.checkedBlocks = j["checked_blocks"] as? Int ?? 0
            g.mismatches = j["mismatches"] as? Int ?? 0
            g.os9Library = j["os9_library"] as? String
            if g.status == "building" { g.problem = "This build didn't finish. Use Grow again to complete it." }
            if g.status == "mismatch" { g.problem = "Some blocks didn't match the interpreter, so this library can't be trusted yet." }
            found.append(g)
        }
        for g in games where g.building || g.queued {
            if let i = found.firstIndex(where: { $0.id == g.id }) { found[i] = g } else { found.append(g) }
        }
        games = found.sorted { $0.title < $1.title }
        countWaiting()
    }

    private func update(_ id: String, _ change: (inout Game) -> Void) {
        if let i = games.firstIndex(where: { $0.id == id }) { change(&games[i]) }
    }

    /// A dropped ROM goes straight into the queue, under a name until inspection gives the real one.
    func add(_ url: URL) {
        let key = "drop-" + url.path
        if !games.contains(where: { $0.romPath == url.path && ($0.building || $0.queued) }) {
            var g = Game(id: key, title: url.deletingPathExtension().lastPathComponent, console: "", romPath: url.path)
            g.queued = true
            g.status = "building"
            games.append(g)
            selection = key
            queue.append(key)
            next()
        }
    }

    func rebuild(_ game: Game) {
        update(game.id) { $0.queued = true; $0.problem = nil; $0.done = []; $0.log = [] }
        queue.append(game.id)
        next()
    }

    private func next() {
        guard !running, let id = queue.first, let g = games.first(where: { $0.id == id }) else { return }
        queue.removeFirst()
        running = true
        update(id) { $0.queued = false; $0.building = true; $0.done = [] }
        let args = ["python3", repo + "/tools/genexia_recompile.py", g.romPath, "--imac", imacHost]
        run(args) { [weak self] line in self?.event(id, line) } finished: { [weak self] code in
            guard let self else { return }
            let current = self.games.first { $0.id == id || $0.romPath == g.romPath }?.id ?? id
            self.update(current) { $0.building = false; $0.step = ""; if code != 0, $0.problem == nil { $0.problem = "The build stopped." } }
            self.running = false
            self.reload()
            self.next()
        }
    }

    private func event(_ id: String, _ line: String) {
        guard let data = line.data(using: .utf8),
              let e = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let name = e["event"] as? String else { return }
        let live = games.first { $0.id == id }?.id ?? games.first { $0.building }?.id ?? id
        switch name {
        case "step":
            update(live) { g in
                if !g.step.isEmpty, g.step != e["step"] as? String { g.done.insert(g.step) }
                g.step = e["step"] as? String ?? ""
                g.detail = e["detail"] as? String ?? ""
                g.log.append(g.detail)
            }
        case "inspected":
            let realID = e["id"] as? String ?? live
            update(live) { g in
                if let t = e["title"] as? String { g.title = t }
                g.console = e["console"] as? String ?? ""
                g.id = realID
            }
            if let x = explorers[live] { explorers[realID] = x }
            games.removeAll { $0.id == realID && !$0.building }
            if selection == live { selection = realID }
        case "map":
            if let path = e["path"] as? String { explorer(for: live).load(URL(fileURLWithPath: path)) }
        case "stats":
            update(live) { g in
                g.instructions = e["instructions"] as? Int ?? g.instructions
                g.log.append("Compiled \(g.instructions.formatted()) instructions")
            }
        case "round":
            update(live) { g in
                g.nativePercent = (e["native_percent"] as? NSNumber)?.doubleValue ?? 0
                g.log.append("Round \(e["round"] ?? ""): \(e["new"] ?? 0) new addresses, \(g.nativePercent)% native")
            }
        case "recorded":
            update(live) { g in g.log.append("\(e["new"] ?? 0) new addresses recorded on the iMac") }
        case "measured":
            update(live) { g in
                g.nativePercent = (e["native_percent"] as? NSNumber)?.doubleValue ?? 0
                g.mode7Percent = (e["mode7_percent"] as? NSNumber)?.doubleValue ?? 0
                g.log.append("Measured \(g.nativePercent)% native")
            }
        case "verified":
            update(live) { g in
                g.checkedBlocks = e["blocks"] as? Int ?? 0
                g.mismatches = e["mismatches"] as? Int ?? 0
                g.log.append("Checked \(g.checkedBlocks.formatted()) blocks, \(g.mismatches) mismatches")
            }
        case "done":
            update(live) { g in if !g.step.isEmpty { g.done.insert(g.step) }; g.log.append("Finished in \(e["seconds"] ?? 0) s") }
        case "failed":
            update(live) { g in g.problem = e["message"] as? String }
        default: break
        }
    }

    private var explorers: [String: RomExplorer] = [:]
    func explorer(for id: String) -> RomExplorer {
        if let x = explorers[id] { return x }
        let x = RomExplorer()
        explorers[id] = x
        x.load(gamesDir.appendingPathComponent(id).appendingPathComponent("map.json"), instant: true)
        return x
    }

    func setOptions(_ game: Game, _ options: GameOptions) {
        update(game.id) { $0.options = options }
        options.save(to: gamesDir.appendingPathComponent(game.id).appendingPathComponent("options.txt"))
    }

    func play(_ game: Game) {
        run([repo + "/genexia", "--rom", game.romPath], onLine: { _ in }, finished: { _ in })
    }

    func reveal(_ game: Game) {
        NSWorkspace.shared.open(gamesDir.appendingPathComponent(game.id))
    }

    /// Makes a double-clickable app of the game: emulators, ROM and native code together.
    func package(_ game: Game) {
        banner = "Packaging \(game.title)…"
        packaging = game.id
        var app: String?
        var problem: String?
        run(["python3", repo + "/tools/package_game.py", game.id], onLine: { [weak self] line in
            guard let e = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any] else { return }
            if let s = e["step"] as? String { self?.banner = s + "…" }
            if let a = e["app"] as? String { app = a }
            if let m = e["message"] as? String { problem = m }
        }) { [weak self] code in
            self?.packaging = nil
            if code == 0, let app {
                let url = URL(fileURLWithPath: app)
                self?.banner = "\(url.deletingPathExtension().lastPathComponent) is ready in Applications › Genexia Games."
                NSWorkspace.shared.activateFileViewerSelecting([url])
            } else {
                self?.banner = problem ?? "Packaging \(game.title) failed."
            }
        }
    }

    func sendToIMac(_ game: Game) {
        guard let lib = game.os9Library else { return }
        banner = "Sending \(game.title) to the iMac…"
        run(["python3", repo + "/tools/os9_share.py", imacHost, "push", lib, game.id + ".lib"], onLine: { _ in }) { [weak self] code in
            self?.banner = code == 0 ? "\(game.title) is on the iMac." : "Couldn't reach the iMac at \(self?.imacHost ?? "")."
        }
    }

    private func run(_ args: [String], onLine: @escaping @MainActor (String) -> Void, finished: @escaping @MainActor (Int32) -> Void) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        p.arguments = args
        p.currentDirectoryURL = URL(fileURLWithPath: repo)
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin"
        p.environment = env
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = FileHandle.nullDevice
        var pending = Data()
        pipe.fileHandleForReading.readabilityHandler = { h in
            let chunk = h.availableData
            if chunk.isEmpty { h.readabilityHandler = nil; return }
            pending.append(chunk)
            while let n = pending.firstIndex(of: 10) {
                let line = String(decoding: pending[pending.startIndex..<n], as: UTF8.self)
                pending.removeSubrange(pending.startIndex...n)
                Task { @MainActor in onLine(line) }
            }
        }
        p.terminationHandler = { q in
            let code = q.terminationStatus
            Task { @MainActor in finished(code) }
        }
        do { try p.run() } catch { Task { @MainActor in finished(-1) } }
    }
}
