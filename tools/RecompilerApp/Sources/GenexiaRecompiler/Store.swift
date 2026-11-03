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
    var reachedPercent: Double = 0
    var blocks = 0

    // a build in progress
    var building = false
    var queued = false
    var step = ""
    var detail = ""
    var done: Set<String> = []
    var log: [String] = []
    var progressLabel = ""
    var progressDone = 0
    var progressTotal = 0
    var progressStart = Date()

    var isN64: Bool { console == "Nintendo 64" }
    var isPSP: Bool { console == "PSP" }
    var encrypted: Bool { problem?.hasPrefix("This disc's program is encrypted") == true }
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

let pspBuildSteps: [(id: String, title: String, icon: String)] = [
    ("inspect", "Inspect", "magnifyingglass"),
    ("discover", "Find code", "point.topleft.down.curvedto.point.bottomright.up"),
    ("compile", "Compile", "cpu"),
    ("link", "Link", "link"),
]

enum InfoPage { case library, how, systems }

enum GameTab { case compile, play }

/// A file holding someone's progress in a game.
struct GameSave: Identifiable {
    let url: URL
    let title: String
    let detail: String
    let icon: String
    let date: Date
    var id: URL { url }
}

/// A packaging run, for the window that shows it.
struct PackageJob: Identifiable {
    let id = UUID()
    let title: String
    let target: PackageTarget
    var psp = false
    var step = ""
    var done = 0
    var total = 0
    var bytes = 0
    var finished = false
    var result: URL?
    var problem: String?
}

/// The machines a game can be packaged for.
enum PackageTarget: String, CaseIterable, Identifiable {
    case arm, intel, ppc, psp, windows, linux
    var id: String { rawValue }
    var title: String {
        switch self {
        case .arm: return "Mac Apple Silicon"
        case .intel: return "Mac Intel"
        case .ppc: return "Mac PowerPC: Mac OS 9 disk for G3, G4 (G5 in Classic)"
        case .psp: return "Sony PSP: folder for PSP/GAME, SNES games"
        case .windows: return "Windows PC (64-bit): folder, PSP games"
        case .linux: return "Linux PC (64-bit): folder, PSP games, uses the system's SDL2"
        }
    }
    func available(_ g: Game) -> Bool {
        switch self {
        case .ppc: return g.os9Library != nil
        case .psp: return g.console == "SNES"
        case .arm: return !g.isPSP || g.instructions > 0
        case .windows, .linux: return g.isPSP && g.instructions > 0
        default: return !g.isPSP
        }
    }
    var steps: [(id: String, title: String, icon: String)] {
        switch self {
        case .arm: return [("runtime", "Build the emulators", "cpu"), ("icon", "Make the icon", "photo"), ("assemble", "Put the app together", "shippingbox")]
        case .intel: return [("runtime", "Build the Intel emulators", "cpu"), ("compile", "Compile the game for Intel", "hammer"), ("link", "Link its library", "link"),
                             ("icon", "Make the icon", "photo"), ("assemble", "Put the app together", "shippingbox")]
        case .ppc: return [("runtime", "Build the Mac OS 9 emulators", "cpu"), ("disk", "Write the disk", "opticaldisc")]
        case .psp: return [("compile", "Compile the game and chips for PSP", "hammer"), ("assemble", "Put the folder together", "shippingbox")]
        case .windows: return [("compile", "Compile the game for Windows", "hammer"), ("runtime", "Build the player", "cpu"), ("icon", "Make the icon", "photo"),
                               ("assemble", "Put the folder together", "shippingbox"), ("disc", "Copy the disc", "opticaldisc")]
        case .linux: return [("compile", "Compile the game for Linux", "hammer"), ("runtime", "Build the player", "cpu"),
                             ("assemble", "Put the folder together", "shippingbox"), ("disc", "Copy the disc", "opticaldisc")]
        }
    }
}

/// What Genexia applies when this game starts, kept in options.txt beside its library.
struct GameOptions: Equatable {
    var picture = 0     // 0 square pixels, 1 4:3 TV, 2 16:9, 3 ultrawide
    var filter = 0      // sharp, smooth, scanlines, CRT
    var sprites = 1     // 1 removes the hardware's per-line limit
    var runahead = 0
    var speed = 100
    var record = 1
    var scale = 4       // PSP: times its resolution
    var msaa = 4        // PSP: anti-aliasing samples
    var window = 1      // PSP: 0 960x544, 1 1920x1088, 2 full screen
    var fps = 0         // PSP: 0 the game's own pace, or frames a second for one that draws every second vblank

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
            case "scale": scale = v
            case "msaa": msaa = v
            case "window": window = v
            case "fps": fps = v
            default: break
            }
        }
    }

    func save(to file: URL) {
        let t = "picture \(picture)\nfilter \(filter)\nsprites \(sprites)\nrunahead \(runahead)\nspeed \(speed)\nrecord \(record)\nscale \(scale)\nmsaa \(msaa)\nwindow \(window)\nfps \(fps)\n"
        try? t.write(to: file, atomically: true, encoding: .utf8)
    }
}

@MainActor
final class Store: ObservableObject {
    @Published var games: [Game] = []
    @Published var selection: String?
    @Published var banner: String?
    @Published var packaging: PackageJob?
    @Published var info: InfoPage? = .library
    @Published var art: [String: NSImage] = [:]      // box art, or a label someone chose
    @Published var shots: [String: NSImage] = [:]    // a moment of the game itself
    @Published var removing: Game?
    @Published var tab = GameTab.compile
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
        findDiscs()
        Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.findDiscs() }
        }
        Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.countWaiting(); self?.fetchArt() }
        }
    }

    private func addresses(_ url: URL) -> Set<Substring> {
        guard let t = try? String(contentsOf: url, encoding: .utf8) else { return [] }
        return Set(t.split(separator: "\n").filter { $0.count == 8 || $0.hasPrefix("B ") })
    }

    /// Genexia adds to recorded.txt during play, so this keeps up while a game runs.
    func countWaiting() {
        for i in games.indices where !games[i].building && !games[i].id.hasPrefix("drop-") {
            let d = gamesDir.appendingPathComponent(games[i].id)
            let played = addresses(d.appendingPathComponent("recorded.txt")).union(addresses(d.appendingPathComponent("imac-recorded.txt")))
            let new = played.isEmpty ? [] : played.subtracting(addresses(d.appendingPathComponent("coverage.txt")))
            if games[i].waiting != new.count {
                games[i].waiting = new.count
                explorer(for: games[i].id).setWaiting(Array(new))
            }
        }
    }

    private var findingDiscs = false

    /// PSP discs in Downloads or on the Desktop come onto the shelf by themselves, ready to compile.
    func findDiscs() {
        guard !findingDiscs else { return }
        findingDiscs = true
        var found: [String] = []
        run(["python3", repo + "/tools/psp_game.py", "scan"], onLine: { found.append($0) }) { [weak self] _ in
            guard let self else { return }
            self.findingDiscs = false
            guard !found.isEmpty else { return }
            if !self.games.contains(where: { $0.building || $0.queued }) { self.explorers = [:] }
            self.reload()
            for line in found where line.hasPrefix("dumped ") {
                if let g = self.games.first(where: { $0.id == line.dropFirst(7) }) { self.rebuild(g) }
            }
        }
    }

    /// Genexia holds no decryption code: PPSSPP dumps the player's own game, and the look for
    /// discs picks the dump up, soonest when PPSSPP closes.
    func dumpWithPPSSPP(_ game: Game) {
        let places = ["/Applications", NSHomeDirectory() + "/Applications"].flatMap { folder in
            ["PPSSPPSDL", "PPSSPP"].map { "\(folder)/\($0).app/Contents/MacOS/\($0)" }
        }
        guard let ppsspp = places.first(where: FileManager.default.isExecutableFile) else {
            banner = "Dumping needs PPSSPP, which isn't installed."
            return
        }
        run([ppsspp, game.romPath], onLine: { _ in }) { [weak self] _ in self?.findDiscs() }
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
            g.reachedPercent = (j["reached_percent"] as? NSNumber)?.doubleValue ?? 0
            g.blocks = j["blocks"] as? Int ?? 0
            g.problem = j["problem"] as? String
            if g.status == "building" { g.problem = "This build didn't finish. Use \(g.isPSP ? "Compile" : "Grow again") to complete it." }
            if g.status == "mismatch" { g.problem = "Some blocks didn't match the interpreter, so this library can't be trusted yet." }
            found.append(g)
        }
        for g in games where g.building || g.queued {
            if let i = found.firstIndex(where: { $0.id == g.id }) { found[i] = g } else { found.append(g) }
        }
        games = found.sorted { $0.title < $1.title }
        countWaiting()
        for g in games { loadArt(g.id) }
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
            info = nil
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
        let args = g.romPath.lowercased().hasSuffix(".iso")
            ? ["python3", repo + "/tools/psp_game.py", "build", g.romPath]
            : ["python3", repo + "/tools/genexia_recompile.py", g.romPath, "--imac", imacHost]
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
        case "progress":
            update(live) { g in
                let label = e["label"] as? String ?? ""
                if label != g.progressLabel || (e["done"] as? Int ?? 0) == 0 { g.progressStart = Date() }
                g.progressLabel = label
                g.progressDone = e["done"] as? Int ?? 0
                g.progressTotal = e["total"] as? Int ?? 0
            }
            if e["label"] as? String == "Compiling", let done = e["done"] as? Int, let total = e["total"] as? Int, total > 0 {
                explorer(for: live).compiling(Double(done) / Double(total))
            }
        case "timing":
            update(live) { g in g.log.append("\(e["detail"] ?? ""): \(e["seconds"] ?? 0) s") }
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
            explorer(for: live).compiling(1)
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
        x.load(gamesDir.appendingPathComponent(id).appendingPathComponent("map.json"), instant: true,
               native: (games.first { $0.id == id }?.instructions ?? 0) > 0)
        return x
    }

    private var factsCache: [String: [RomFact]] = [:]
    func facts(for game: Game) -> [RomFact] {
        if let f = factsCache[game.id] { return f }
        let f = romFacts(path: game.romPath, console: game.console)
        if !f.isEmpty { factsCache[game.id] = f }
        return f
    }

    /// The latest picture from each bot while the bots play.
    func botScreens(_ game: Game) -> [URL] {
        let tmp = gamesDir.appendingPathComponent(game.id).appendingPathComponent("tmp")
        let all = (try? FileManager.default.contentsOfDirectory(at: tmp, includingPropertiesForKeys: nil)) ?? []
        return all.filter { $0.pathExtension == "ppm" }.sorted { $0.path < $1.path }
    }

    func setOptions(_ game: Game, _ options: GameOptions) {
        update(game.id) { $0.options = options }
        options.save(to: gamesDir.appendingPathComponent(game.id).appendingPathComponent("options.txt"))
    }

    func play(_ game: Game) {
        if game.isPSP { return playPSP(game) }
        run([repo + "/genexia", "--rom", game.romPath], onLine: { _ in }, finished: { _ in })
    }

    /// A disc that isn't compiled yet compiles first; press Play again when it's done.
    private func playPSP(_ game: Game) {
        guard game.instructions > 0 else { return rebuild(game) }
        let d = gamesDir.appendingPathComponent(game.id), o = game.options
        var settings = ["PSP_SCALE": String(o.scale), "PSP_MSAA": String(o.msaa)]
        if o.window == 2 { settings["PSP_FULLSCREEN"] = "1" }
        if o.fps > 0 { settings["PSP_FPS"] = String(o.fps) }
        settings["PSP_SIZE"] = o.window == 0 ? "960x544" : "1920x1088"
        run([d.appendingPathComponent("play_metal").path, "game.elf", game.romPath, "999999"], folder: d, settings: settings,
            onLine: { _ in }, finished: { [weak self] code in
                if code != 0 { self?.banner = "\(game.title.capitalized) stopped unexpectedly." }
            })
    }

    func reveal(_ game: Game) {
        NSWorkspace.shared.open(gamesDir.appendingPathComponent(game.id))
    }

    private func loadArt(_ id: String) {
        let d = gamesDir.appendingPathComponent(id)
        art[id] = NSImage(contentsOf: d.appendingPathComponent("art.png"))
        shots[id] = NSImage(contentsOf: d.appendingPathComponent("shot.png"))
    }

    private var fetchingArt = false
    private var artTried: Set<String> = []
    /// Gets each finished game its pictures, one at a time.
    func fetchArt() {
        guard !fetchingArt, let g = games.first(where: { g in
            let d = gamesDir.appendingPathComponent(g.id).path
            let fm = FileManager.default
            return !g.building && !g.queued && g.instructions > 0 && !artTried.contains(g.id)
                && (!fm.fileExists(atPath: d + "/shot.png") || !fm.fileExists(atPath: d + "/art.png") && !fm.fileExists(atPath: d + "/art.none"))
        }) else { return }
        fetchingArt = true
        artTried.insert(g.id)
        run(["python3", repo + "/tools/game_art.py", g.id], onLine: { _ in }) { [weak self] _ in
            self?.fetchingArt = false
            self?.loadArt(g.id)
        }
    }

    func setArt(_ game: Game, from url: URL) {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        guard let image = NSImage(contentsOf: url), let tiff = image.tiffRepresentation,
              let png = NSBitmapImageRep(data: tiff)?.representation(using: .png, properties: [:]) else {
            banner = "That file isn't a picture Genexia can read."
            return
        }
        try? png.write(to: gamesDir.appendingPathComponent(game.id).appendingPathComponent("art.png"))
        loadArt(game.id)
    }

    /// The label goes back to a moment of the game itself.
    func clearArt(_ game: Game) {
        let d = gamesDir.appendingPathComponent(game.id)
        try? FileManager.default.removeItem(at: d.appendingPathComponent("art.png"))
        FileManager.default.createFile(atPath: d.appendingPathComponent("art.none").path, contents: nil)
        loadArt(game.id)
    }

    func cartridge(_ game: Game, big: Bool = false) -> Cartridge {
        Cartridge(console: game.console, label: art[game.id] ?? shots[game.id],
                  title: big && art[game.id] == nil && shots[game.id] != nil ? game.title.capitalized : nil)
    }

    /// The app icon is the game's cartridge. package_game.py picks this file up.
    private func writeIcon(_ game: Game) {
        guard art[game.id] != nil || shots[game.id] != nil else { return }
        let r = ImageRenderer(content: cartridge(game, big: true).frame(width: 1024, height: 1024))
        guard let tiff = r.nsImage?.tiffRepresentation,
              let png = NSBitmapImageRep(data: tiff)?.representation(using: .png, properties: [:]) else { return }
        try? png.write(to: gamesDir.appendingPathComponent(game.id).appendingPathComponent("cartridge.png"))
    }

    /// Makes a game that runs on its own: emulators, ROM and native code together.
    func package(_ game: Game, for target: PackageTarget) {
        packaging = PackageJob(title: game.title.capitalized, target: target, psp: game.isPSP)
        writeIcon(game)
        run(["python3", repo + "/tools/package_game.py", game.id, "--target", target.rawValue], onLine: { [weak self] line in
            guard let e = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any] else { return }
            if let id = e["id"] as? String {
                self?.packaging?.step = id
                self?.packaging?.done = e["done"] as? Int ?? 0
                self?.packaging?.total = e["total"] as? Int ?? 0
            }
            if let a = e["app"] as? String { self?.packaging?.result = URL(fileURLWithPath: a) }
            if let b = e["bytes"] as? Int { self?.packaging?.bytes = b }
            if let m = e["message"] as? String { self?.packaging?.problem = m }
        }) { [weak self] code in
            guard let self, self.packaging != nil else { return }
            self.packaging?.finished = true
            if code != 0 || self.packaging?.result == nil, self.packaging?.problem == nil {
                self.packaging?.problem = "Packaging stopped before it finished."
            }
        }
    }

    func saves(for game: Game) -> [GameSave] {
        let packaged = gamesDir.deletingLastPathComponent().appendingPathComponent(game.id)
        let places: [(URL, String, String, String)] = [
            (URL(fileURLWithPath: game.romPath + ".state"), "Save state", "From Genexia", "bookmark.fill"),
            (packaged.appendingPathComponent("game.state"), "Save state", "From the packaged app", "bookmark.fill"),
            (packaged.appendingPathComponent("battery.srm"), "Cartridge save", "From the packaged app", "memorychip"),
        ]
        if game.isPSP { return pspSaves(game) }
        return places.compactMap { url, title, detail, icon in
            guard let d = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date else { return nil }
            return GameSave(url: url, title: title, detail: detail, icon: icon, date: d)
        }
    }

    private func pspSaves(_ game: Game) -> [GameSave] {
        let root = gamesDir.appendingPathComponent(game.id).appendingPathComponent("saves")
        let all = (try? FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: nil)) ?? []
        return all.compactMap { url in
            guard let d = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date else { return nil }
            return GameSave(url: url, title: "Game save", detail: url.lastPathComponent, icon: "memorychip", date: d)
        }
    }

    /// To the Trash, not gone: a game can take a long time to build again.
    func remove(_ game: Game, saves withSaves: Bool) {
        guard !game.building, !game.queued else { return }
        let fm = FileManager.default
        if game.isPSP { keepOffShelf(game, saves: !withSaves) }
        let trash = (withSaves ? saves(for: game).map(\.url) : []) + [gamesDir.appendingPathComponent(game.id)]
        for url in trash where fm.fileExists(atPath: url.path) {
            do { try fm.trashItem(at: url, resultingItemURL: nil) } catch {
                banner = "Couldn't remove \(game.title): \(error.localizedDescription)"
                return
            }
        }
        games.removeAll { $0.id == game.id }
        explorers[game.id] = nil
        factsCache[game.id] = nil
        art[game.id] = nil
        shots[game.id] = nil
        if selection == game.id { selection = games.first?.id }
    }

    /// Its disc is still in Downloads, so the next look there mustn't bring it back. Saves live
    /// inside a PSP game's folder, so keeping them means moving them out first.
    private func keepOffShelf(_ game: Game, saves keep: Bool) {
        let support = gamesDir.deletingLastPathComponent(), list = support.appendingPathComponent("psp-removed.txt")
        let earlier = (try? String(contentsOf: list, encoding: .utf8)) ?? ""
        try? (earlier + game.id + "\n").write(to: list, atomically: true, encoding: .utf8)
        let saves = gamesDir.appendingPathComponent(game.id).appendingPathComponent("saves")
        guard keep, FileManager.default.fileExists(atPath: saves.path) else { return }
        let kept = support.appendingPathComponent("psp-saves")
        try? FileManager.default.createDirectory(at: kept, withIntermediateDirectories: true)
        try? FileManager.default.moveItem(at: saves, to: kept.appendingPathComponent(game.id))
    }

    func sendToIMac(_ game: Game) {
        guard let lib = game.os9Library else { return }
        banner = "Sending \(game.title) to the iMac…"
        run(["python3", repo + "/tools/os9_share.py", imacHost, "push", lib, game.id + ".lib"], onLine: { _ in }) { [weak self] code in
            self?.banner = code == 0 ? "\(game.title) is on the iMac." : "Couldn't reach the iMac at \(self?.imacHost ?? "")."
        }
    }

    private func run(_ args: [String], folder: URL? = nil, settings: [String: String] = [:], onLine: @escaping @MainActor (String) -> Void, finished: @escaping @MainActor (Int32) -> Void) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        p.arguments = args
        p.currentDirectoryURL = folder ?? URL(fileURLWithPath: repo)
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin"
        p.environment = env.merging(settings) { $1 }
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
