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
    var interpreterPercent: Double = 0
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
    ("verify", "Verify", "checkmark.seal"),
    ("os9", "iMac library", "desktopcomputer"),
]

@MainActor
final class Store: ObservableObject {
    @Published var games: [Game] = []
    @Published var selection: String?
    @Published var banner: String?
    @AppStorage("imacHost") var imacHost = "192.168.1.38"

    private var queue: [String] = []
    private var running = false

    let repo = Bundle.main.object(forInfoDictionaryKey: "GenexiaRepo") as? String
        ?? FileManager.default.currentDirectoryPath
    var gamesDir: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/Genexia/games")
    }

    init() { reload() }

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
            g.interpreterPercent = j["interpreter_percent"] as? Double ?? 0
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
        let args = ["python3", repo + "/tools/genexia_recompile.py", g.romPath]
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
            games.removeAll { $0.id == realID && !$0.building }
            if selection == live { selection = realID }
        case "stats":
            update(live) { g in
                g.instructions = e["instructions"] as? Int ?? g.instructions
                g.log.append("Compiled \(g.instructions.formatted()) instructions")
            }
        case "round":
            update(live) { g in
                g.interpreterPercent = e["interpreter_percent"] as? Double ?? 0
                g.log.append("Round \(e["round"] ?? ""): \(e["new"] ?? 0) new addresses, \(g.interpreterPercent)% interpreted")
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

    func play(_ game: Game) {
        run([repo + "/genexia", "--rom", game.romPath], onLine: { _ in }, finished: { _ in })
    }

    func reveal(_ game: Game) {
        NSWorkspace.shared.open(gamesDir.appendingPathComponent(game.id))
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
