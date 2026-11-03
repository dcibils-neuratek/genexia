import SwiftUI
import UniformTypeIdentifiers

let accent = Color(red: 0.36, green: 0.62, blue: 1.0)
let romNotice = "Genexia doesn't provide ROMs. You should own the real game and supply your own ROM."
let romExtensions: Set<String> = ["sfc", "smc", "md", "gen", "bin", "z64", "n64", "v64", "iso"]

@main
struct RecompilerApp: App {
    @StateObject private var store = Store()
    init() { NSApplication.shared.setActivationPolicy(.regular) }
    var body: some Scene {
        WindowGroup("Genexia Recompiler") {
            MainView().environmentObject(store)
                .frame(minWidth: 940, minHeight: 760)
                .onAppear { NSApp.activate(ignoringOtherApps: true) }
        }
        .windowStyle(.hiddenTitleBar)
        Settings {
            Form { TextField("iMac address", text: store.$imacHost) }.padding().frame(width: 320)
                .environmentObject(store)
        }
    }
}

struct MainView: View {
    @EnvironmentObject var store: Store
    @State private var targeted = false

    var body: some View {
        HStack(spacing: 0) {
            sidebar.frame(width: 280)
            Divider().opacity(0.3)
            ZStack {
                if store.info == .library {
                    AllGames()
                } else if store.info == .how {
                    HowItWorks()
                } else if store.info == .systems {
                    Systems()
                } else if let g = store.games.first(where: { $0.id == store.selection }) {
                    GameDetail(game: g)
                } else {
                    DropHint(big: true)
                }
                if let b = store.banner {
                    VStack { Spacer(); Text(b).font(.callout).padding(.horizontal, 14).padding(.vertical, 8)
                        .background(.ultraThinMaterial, in: Capsule()).padding(16) }
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background(LinearGradient(colors: [Color(white: 0.09), Color(red: 0.07, green: 0.09, blue: 0.14)],
                                   startPoint: .topLeading, endPoint: .bottomTrailing))
        .preferredColorScheme(.dark)
        .overlay {
            if targeted {
                RoundedRectangle(cornerRadius: 14).strokeBorder(accent, style: StrokeStyle(lineWidth: 3, dash: [9, 6]))
                    .padding(8).allowsHitTesting(false)
            }
        }
        .alert("Remove \(store.removing?.title.capitalized ?? "game")?", isPresented: Binding(get: { store.removing != nil }, set: { if !$0 { store.removing = nil } }), presenting: store.removing) { game in
            Button("Remove Game", role: .destructive) { store.remove(game, saves: false) }
            if !store.saves(for: game).isEmpty {
                Button("Remove Game and Saves", role: .destructive) { store.remove(game, saves: true) }
            }
            Button("Cancel", role: .cancel) {}
        } message: { _ in
            Text("Its compiled code and found-code list go to the Trash, and it would have to be built again. Your ROM file and any packaged apps stay where they are.")
        }
        .sheet(item: $store.packaging) { job in PackageSheet(job: job).environmentObject(store).interactiveDismissDisabled(!job.finished) }
        .onDrop(of: [UTType.fileURL], isTargeted: $targeted) { items in
            for item in items {
                _ = item.loadObject(ofClass: URL.self) { url, _ in
                    guard let url, romExtensions.contains(url.pathExtension.lowercased()) else { return }
                    Task { @MainActor in store.add(url) }
                }
            }
            return true
        }
    }

    var sidebar: some View {
        VStack(alignment: .leading, spacing: 0) {
            VStack(alignment: .leading, spacing: 2) {
                Text("Genexia").font(.system(size: 22, weight: .bold, design: .rounded))
                Text("Recompiler").font(.subheadline).foregroundStyle(.secondary)
            }
            .padding(.horizontal, 18).padding(.top, 38).padding(.bottom, 10)
            VStack(spacing: 2) {
                InfoButton(page: .library, title: "All Games", icon: "rectangle.stack.fill")
                InfoButton(page: .how, title: "How It Works", icon: "sparkles.rectangle.stack")
                InfoButton(page: .systems, title: "Systems", icon: "cpu")
            }.padding(.horizontal, 10).padding(.bottom, 8)
            ScrollView {
                LazyVStack(spacing: 4) {
                    ForEach(store.games) { g in GameRow(game: g) }
                }.padding(.horizontal, 10)
            }
            DropHint(big: false).padding(12)
        }
        .background(Color.black.opacity(0.22))
    }
}

struct DropHint: View {
    let big: Bool
    @State private var picking = false
    @EnvironmentObject var store: Store
    var body: some View {
        Button { picking = true } label: {
            VStack(spacing: big ? 12 : 4) {
                Image(systemName: "square.and.arrow.down").font(.system(size: big ? 44 : 16, weight: .light)).foregroundStyle(accent)
                Text(big ? "Drop a ROM or disc here" : "Drop or choose a ROM").font(big ? .title2.weight(.medium) : .caption)
                if big {
                    Text("SNES, Nintendo 64 and PSP today. PSP discs in Downloads or on the Desktop appear by themselves. Genesis is recognized and coming next.").font(.callout).foregroundStyle(.secondary)
                    Text(romNotice).font(.caption).foregroundStyle(.tertiary).multilineTextAlignment(.center)
                }
            }
            .frame(maxWidth: big ? 420 : .infinity).padding(big ? 44 : 12)
            .background(RoundedRectangle(cornerRadius: 14).strokeBorder(.white.opacity(0.14), style: StrokeStyle(lineWidth: 1, dash: [6, 5])))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .fileImporter(isPresented: $picking, allowedContentTypes: [.data], allowsMultipleSelection: true) { r in
            for u in (try? r.get()) ?? [] { store.add(u) }
        }
    }
}

struct InfoButton: View {
    @EnvironmentObject var store: Store
    let page: InfoPage
    let title: String
    let icon: String
    var body: some View {
        Button { store.info = page } label: {
            Label(title, systemImage: icon)
                .font(.system(size: 13, weight: .medium)).padding(9).frame(maxWidth: .infinity, alignment: .leading)
                .background(store.info == page ? accent.opacity(0.2) : .clear, in: RoundedRectangle(cornerRadius: 9))
                .contentShape(Rectangle())
        }.buttonStyle(.plain)
    }
}

struct ConsoleBadge: View {
    let console: String
    var color: Color { console == "SNES" ? .purple : console == "Genesis" ? .blue : console == "PSP" ? .orange : console.isEmpty ? .gray : .green }
    var body: some View {
        Text(console.isEmpty ? "?" : console == "Nintendo 64" ? "N64" : console)
            .font(.system(size: 10, weight: .bold, design: .rounded))
            .padding(.horizontal, 6).padding(.vertical, 2)
            .background(color.opacity(0.28), in: Capsule())
    }
}

struct GameRow: View {
    @EnvironmentObject var store: Store
    let game: Game
    var body: some View {
        Button { store.selection = game.id; store.info = nil } label: {
            HStack(spacing: 10) {
                Group {
                    if game.building { ProgressView().controlSize(.small) }
                    else if game.queued { Image(systemName: "clock").foregroundStyle(.secondary) }
                    else if game.problem != nil { Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange) }
                    else { Image(systemName: "checkmark.circle.fill").foregroundStyle(.green) }
                }.frame(width: 20)
                store.cartridge(game).frame(width: 28, height: 28)
                VStack(alignment: .leading, spacing: 3) {
                    Text(game.title.capitalized).font(.system(size: 13, weight: .medium)).lineLimit(1)
                    ConsoleBadge(console: game.console)
                }
                Spacer()
                if game.waiting > 0 {
                    Text("+\(game.waiting)").font(.system(size: 11, weight: .bold, design: .rounded)).monospacedDigit()
                        .padding(.horizontal, 7).padding(.vertical, 3).background(Color.orange.opacity(0.85), in: Capsule())
                        .help("New code found by playing, waiting to be compiled")
                }
            }
            .padding(9)
            .background(store.selection == game.id && store.info == nil ? accent.opacity(0.2) : .clear, in: RoundedRectangle(cornerRadius: 9))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .contextMenu {
            Button("Remove Game…", role: .destructive) { store.removing = game }.disabled(game.building || game.queued)
        }
    }
}

struct GameDetail: View {
    @EnvironmentObject var store: Store
    let game: Game

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            VStack(alignment: .leading, spacing: 14) {
                HStack(spacing: 10) {
                    store.cartridge(game).frame(width: 38, height: 38)
                    Text(game.title.capitalized).font(.system(size: 28, weight: .bold, design: .rounded)).lineLimit(1)
                    ConsoleBadge(console: game.console)
                    Spacer()
                }
                Picker("", selection: $store.tab) {
                    Text("Compile").tag(GameTab.compile)
                    Text("Play").tag(GameTab.play)
                }
                .pickerStyle(.segmented).labelsHidden().frame(maxWidth: 240)
            }
            .padding(.horizontal, 28).padding(.top, 34).padding(.bottom, 12)
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    if store.tab == .compile { compileTab } else { playTab }
                }
                .padding(.horizontal, 28).padding(.top, 6).padding(.bottom, 12)
            }
            if store.tab == .compile { compileBar }
        }
    }

    @ViewBuilder var compileTab: some View {
        if game.building || game.instructions == 0 { StepStrip(game: game) }
        if game.building { BuildProgress(game: game) }
        if game.building && game.step == "play" { BotScreens(game: game) }
        if let p = game.problem {
            Label(p, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                .padding(12).frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.orange.opacity(0.1), in: RoundedRectangle(cornerRadius: 10))
        }
        if game.isPSP { pspCards } else { nativeCards }
        if !game.log.isEmpty {
            Text(game.log.joined(separator: "\n")).font(.system(size: 11, design: .monospaced)).foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled)
                .padding(10).background(Color.black.opacity(0.25), in: RoundedRectangle(cornerRadius: 10))
        }
    }

    @ViewBuilder var pspCards: some View {
        PSPStatus(game: game)
        ExplorerView(explorer: store.explorer(for: game.id), building: game.building, nativePercent: game.reachedPercent, facts: store.facts(for: game), program: true)
        LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 10), count: 3), spacing: 10) {
            Stat(value: game.blocks == 0 ? "–" : game.blocks.formatted(), label: "blocks of native code")
            Stat(value: game.instructions == 0 ? "–" : game.instructions.formatted(), label: "instructions compiled")
            Stat(value: size(game.libraryBytes), label: "Mac program")
        }
    }

    @ViewBuilder var nativeCards: some View {
        StatusCard(game: game)
        if game.waiting > 0 || !game.isN64 { WaitingCard(game: game) }
        ExplorerView(explorer: store.explorer(for: game.id), building: game.building, nativePercent: game.nativePercent, facts: store.facts(for: game))
        LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 10), count: 3), spacing: 10) {
            if game.isN64 {
                Stat(value: game.addresses.formatted(), label: "code addresses seen running")
            } else {
                Stat(value: game.followed == 0 ? "–" : "\(game.followed * 100 / max(1, game.instructions))%", label: "seen running or followed")
                Stat(value: game.guessed == 0 ? "–" : "\(game.guessed * 100 / max(1, game.instructions))%", label: "guessed, not yet reached")
            }
            Stat(value: size(game.libraryBytes), label: "Mac library")
            if !game.isN64 { Stat(value: size(game.os9Bytes), label: "iMac library") }
        }
    }

    var compileBar: some View {
        HStack {
            Button { store.rebuild(game) } label: {
                Label(!game.isPSP ? "Grow again" : game.instructions > 0 ? "Compile again" : "Compile", systemImage: "arrow.triangle.2.circlepath")
            }
                .disabled(game.building || game.queued)
                .help(game.isPSP ? "Recompiles the game with the latest Genexia" : "Keeps everything found so far, adds iMac recordings, sends the bots to play again, recompiles and re-verifies")
            if !game.isN64 && !game.isPSP {
                Button { store.sendToIMac(game) } label: { Label("Send to iMac", systemImage: "desktopcomputer") }
                    .disabled(game.os9Library == nil || game.building)
            }
            Spacer()
            if !game.isPSP {
                Menu {
                    ForEach(PackageTarget.allCases) { t in
                        Button(t.title) { store.package(game, for: t) }.disabled(!t.available(game))
                    }
                } label: { Label("Package App", systemImage: "shippingbox") }
                    .fixedSize()
                    .disabled(game.building || game.queued || store.packaging != nil)
                    .help("Make an app of this game that runs on its own, for the Mac you choose")
            }
            Button { store.reveal(game) } label: { Image(systemName: "folder") }.help("Show the library in Finder")
            Button { store.removing = game } label: { Image(systemName: "trash") }
                .disabled(game.building || game.queued).help("Remove this game from Genexia")
        }
        .padding(.horizontal, 28).padding(.vertical, 14)
    }

    @ViewBuilder var playTab: some View {
        HStack(spacing: 14) {
            Button { store.play(game) } label: {
                Label("Play", systemImage: "play.fill").font(.system(size: 16, weight: .semibold)).padding(.horizontal, 22).padding(.vertical, 8)
            }
            .buttonStyle(.borderedProminent).tint(accent).disabled(game.building || game.problem != nil)
            Text(game.isPSP ? (game.instructions > 0 ? "Native code with Metal graphics" : "Compiles first")
                 : game.nativePercent > 0 ? String(format: "%.1f%% native", game.nativePercent) : "Not measured yet")
                .font(.callout).foregroundStyle(.secondary)
            Spacer()
        }
        if game.isPSP { PSPOptions(game: game) } else { OptionsCard(game: game) }
        SavesCard(game: game)
    }

    func compact(_ n: Int) -> String {
        n >= 1_000_000 ? String(format: "%.1fM", Double(n) / 1_000_000) : n.formatted()
    }
    func size(_ n: Int) -> String {
        n == 0 ? "–" : String(format: "%.1f MB", Double(n) / 1_048_576)
    }
}

/// Where a PSP game stands: discs compile in one go, with no bots or checks against an emulator.
struct PSPStatus: View {
    let game: Game
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(game.building ? "Building" : game.instructions > 0 ? "Ready to play" : "Nothing compiled yet")
                .font(.system(size: 19, weight: .bold, design: .rounded))
            StatusRow(state: game.instructions > 0 ? .done : .open, text: game.instructions > 0
                      ? "\(game.instructions.formatted()) instructions compiled, \(String(format: "%.1f", game.reachedPercent))% of the program"
                      : "Compile turns this disc's program into native code")
            Text("The rest of the program is data, or code the game loads itself, which runs on the built-in interpreter. Compile again after updating Genexia.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .padding(16).frame(maxWidth: .infinity, alignment: .leading)
        .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
    }
}

/// How a PSP game is drawn on this Mac. Applies the next time it starts.
struct PSPOptions: View {
    @EnvironmentObject var store: Store
    let game: Game

    func row(_ title: String, _ path: WritableKeyPath<GameOptions, Int>, _ choices: [(Int, String)]) -> some View {
        Picker(title, selection: Binding(get: { game.options[keyPath: path] }, set: {
            var o = game.options
            o[keyPath: path] = $0
            store.setOptions(game, o)
        })) {
            ForEach(choices, id: \.0) { Text($0.1).tag($0.0) }
        }
        .pickerStyle(.segmented)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Options for this game").font(.headline)
            row("Resolution", \.scale, [(2, "960×544"), (4, "1920×1088"), (6, "2880×1632"), (8, "3840×2176")])
            row("Anti-aliasing", \.msaa, [(1, "Off"), (2, "2×"), (4, "4×")])
            row("Window", \.window, [(0, "Small"), (1, "Large"), (2, "Full screen")])
            Text("Applies the next time the game starts. Arrows D-pad, Z Cross, X Circle, A Square, S Triangle, Q and W shoulders, Return Start, Tab Select, or a gamepad. Cmd-F switches full screen.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .padding(14).background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
    }
}

/// Share of the game's CPU time in compiled code against the interpreter.
/// Where the game stands on its way to native code, in one look.
struct StatusCard: View {
    let game: Game

    var compiled: Bool { game.instructions > 0 }
    var verified: Bool { game.checkedBlocks > 0 && game.mismatches == 0 }
    var measured: Bool { game.nativePercent > 0 }
    var allNative: Bool { game.nativePercent >= 99.95 }

    var headline: String {
        if game.building { return "Building" }
        if !compiled { return "Nothing compiled yet" }
        if game.mismatches != 0 { return "Native code doesn't match yet" }
        if !verified { return "Compiled, not verified yet" }
        if allNative { return "Fully native so far" }
        if game.nativePercent >= 99 { return "Almost fully native" }
        return game.nativePercent >= 90 ? "Mostly native" : "Partly native"
    }

    var verifyText: String {
        if game.mismatches != 0 { return "\(game.mismatches.formatted()) differences from the emulator" }
        if !verified { return "Not checked against the emulator yet" }
        return game.isN64 ? "\(game.checkedBlocks.formatted()) frames played the same as the emulator"
                          : "\(game.checkedBlocks.formatted()) blocks gave the same result as the emulator"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 18) {
                NativeRing(percent: game.nativePercent)
                VStack(alignment: .leading, spacing: 8) {
                    Text(headline).font(.system(size: 19, weight: .bold, design: .rounded))
                    StatusRow(state: compiled ? .done : .open,
                              text: compiled ? "\(game.instructions.formatted()) instructions compiled" : "Nothing compiled yet")
                    StatusRow(state: game.mismatches != 0 ? .bad : verified ? .done : .open, text: verifyText)
                    StatusRow(state: !measured ? .open : allNative ? .done : .part,
                              text: !measured ? "Not measured yet" : allNative ? "All play time so far runs native"
                                  : String(format: "%.1f%% of play time runs native, %.1f%% on the emulator", game.nativePercent, 100 - game.nativePercent))
                    StatusRow(state: .open, text: "Parts of the game the bots haven't reached")
                }
                Spacer(minLength: 0)
            }
            Text("Anything not compiled yet runs on the built-in emulator, so the game always works. Only playing it to the end can show all the code it has: bots rarely get that far, so play with recording on.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .padding(16).frame(maxWidth: .infinity, alignment: .leading)
        .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
    }
}

/// What each bot is seeing right now.
struct BotScreens: View {
    @EnvironmentObject var store: Store
    let game: Game
    @State private var screens: [CGImage] = []
    private let timer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Bots playing").font(.headline)
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 8), count: 4), spacing: 8) {
                ForEach(screens.indices, id: \.self) { i in
                    Image(decorative: screens[i], scale: 1).resizable().interpolation(.none).aspectRatio(4 / 3, contentMode: .fit)
                        .clipShape(RoundedRectangle(cornerRadius: 5))
                }
            }
        }
        .padding(14).frame(maxWidth: .infinity, alignment: .leading)
        .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
        .onReceive(timer) { _ in screens = store.botScreens(game).compactMap(ppmImage) }
    }
}

/// A binary PPM, the picture format the runtime writes.
func ppmImage(_ url: URL) -> CGImage? {
    guard let data = try? Data(contentsOf: url), data.count > 16, data.prefix(2) == Data("P6".utf8) else { return nil }
    var at = data.startIndex + 2, numbers: [Int] = []
    while numbers.count < 3 {
        while at < data.endIndex, data[at] == 10 || data[at] == 32 { at += 1 }
        var n = 0
        while at < data.endIndex, data[at] >= 48, data[at] <= 57 { n = n * 10 + Int(data[at] - 48); at += 1 }
        numbers.append(n)
    }
    at += 1
    let w = numbers[0], h = numbers[1]
    guard w > 0, h > 0, data.endIndex - at >= w * h * 3, let source = CGDataProvider(data: data.subdata(in: at..<(at + w * h * 3)) as CFData) else { return nil }
    return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 24, bytesPerRow: w * 3, space: CGColorSpaceCreateDeviceRGB(),
                   bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue), provider: source, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
}

struct NativeRing: View {
    let percent: Double
    var body: some View {
        ZStack {
            Circle().stroke(.white.opacity(0.1), lineWidth: 10)
            Circle().trim(from: 0, to: percent / 100).stroke(Color.green, style: StrokeStyle(lineWidth: 10, lineCap: .round))
                .rotationEffect(.degrees(-90))
            VStack(spacing: 0) {
                Text(percent > 0 ? String(format: "%.1f%%", percent) : "–").font(.system(size: 19, weight: .bold, design: .rounded)).monospacedDigit()
                Text("native").font(.caption2).foregroundStyle(.secondary)
            }
        }
        .frame(width: 104, height: 104).padding(5)
        .animation(.easeOut(duration: 0.5), value: percent)
    }
}

struct StatusRow: View {
    enum State { case done, part, open, bad }
    let state: State
    let text: String
    var body: some View {
        Label {
            Text(text).font(.callout)
        } icon: {
            switch state {
            case .done: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
            case .part: Image(systemName: "circle.lefthalf.filled").foregroundStyle(.orange)
            case .open: Image(systemName: "circle.dashed").foregroundStyle(.secondary)
            case .bad: Image(systemName: "xmark.octagon.fill").foregroundStyle(.red)
            }
        }
    }
}

struct WaitingCard: View {
    @EnvironmentObject var store: Store
    let game: Game
    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: game.waiting > 0 ? "sparkles" : "record.circle")
                .font(.system(size: 22)).foregroundStyle(game.waiting > 0 ? .orange : .secondary)
            VStack(alignment: .leading, spacing: 2) {
                Text(game.waiting > 0 ? "\(game.waiting.formatted()) new code addresses found by playing" : "No new code waiting")
                    .font(.system(size: 14, weight: .semibold))
                Text(game.waiting > 0 ? "They run in the interpreter until you compile them." : "Play with recording on and anything new shows up here while you play.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            if !game.isN64 { Button("Check iMac") { store.checkIMac(game) }.disabled(game.building || game.queued) }
            if game.waiting > 0 {
                Button("Compile") { store.rebuild(game) }
                    .buttonStyle(.borderedProminent).tint(.orange).disabled(game.building || game.queued)
            }
        }
        .padding(14)
        .background(game.waiting > 0 ? Color.orange.opacity(0.12) : Color.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
    }
}

/// How far along the current stage is, and roughly how long it has left.
struct BuildProgress: View {
    let game: Game
    var body: some View {
        TimelineView(.periodic(from: .now, by: 1)) { tl in
            VStack(alignment: .leading, spacing: 7) {
                HStack {
                    Text(game.detail.isEmpty ? "Working…" : game.detail).font(.system(size: 13, weight: .semibold))
                    Spacer()
                    Text(left(tl.date)).font(.caption).foregroundStyle(.secondary).monospacedDigit()
                }
                if game.progressTotal > 0 {
                    ProgressView(value: Double(game.progressDone), total: Double(game.progressTotal))
                } else {
                    ProgressView().progressViewStyle(.linear)
                }
                Text(game.progressTotal > 0 ? "\(game.progressLabel): \(game.progressDone) of \(game.progressTotal)" : game.progressLabel)
                    .font(.caption).foregroundStyle(.secondary).monospacedDigit()
            }
            .padding(14).background(accent.opacity(0.1), in: RoundedRectangle(cornerRadius: 12))
        }
    }
    func left(_ now: Date) -> String {
        let spent = now.timeIntervalSince(game.progressStart)
        guard game.progressTotal > 0, game.progressDone > 0, game.progressDone < game.progressTotal else { return clock(spent) + " so far" }
        return "about " + clock(spent / Double(game.progressDone) * Double(game.progressTotal - game.progressDone)) + " left"
    }
    func clock(_ s: Double) -> String { String(format: "%d:%02d", Int(s) / 60, Int(s) % 60) }
}

/// What this game has saved on this Mac.
struct SavesCard: View {
    @EnvironmentObject var store: Store
    let game: Game
    var body: some View {
        let saves = store.saves(for: game)
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Saves").font(.headline)
                Spacer()
                if !saves.isEmpty {
                    Button { NSWorkspace.shared.activateFileViewerSelecting(saves.map(\.url)) } label: { Image(systemName: "folder") }
                        .help("Show the saves in Finder")
                }
            }
            if saves.isEmpty {
                Text(game.isPSP ? "Nothing saved yet." : "Nothing saved yet. Press F5 in game to save your place and F7 to load it.").font(.caption).foregroundStyle(.secondary)
            }
            ForEach(saves) { s in
                HStack(spacing: 10) {
                    Image(systemName: s.icon).foregroundStyle(accent).frame(width: 20)
                    VStack(alignment: .leading, spacing: 1) {
                        Text(s.title).font(.system(size: 13, weight: .medium))
                        Text(s.detail).font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Text(s.date.formatted(date: .abbreviated, time: .shortened)).font(.caption).foregroundStyle(.secondary)
                }
            }
            Text(game.isPSP ? "Saved by the game itself, from its own menus." : "Cartridge saves are kept by packaged apps. Save states work everywhere.").font(.caption).foregroundStyle(.secondary)
        }
        .padding(14).background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
    }
}

/// Packaging a game, step by step.
struct PackageSheet: View {
    @EnvironmentObject var store: Store
    let job: PackageJob
    var steps: [(id: String, title: String, icon: String)] { job.target.steps }
    var current: Int { steps.firstIndex { $0.id == job.step } ?? -1 }
    var succeeded: Bool { job.finished && job.problem == nil }

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            VStack(alignment: .leading, spacing: 4) {
                Text(heading).font(.system(size: 20, weight: .bold, design: .rounded))
                Text(job.target.title).font(.callout).foregroundStyle(.secondary)
            }
            VStack(alignment: .leading, spacing: 0) {
                ForEach(Array(steps.enumerated()), id: \.offset) { i, s in row(i, s) }
            }
            if let p = job.problem {
                Label(p, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
            } else if succeeded, let r = job.result {
                Text("\(r.lastPathComponent), \(String(format: "%.0f MB", Double(job.bytes) / 1_048_576))").font(.callout).foregroundStyle(.secondary)
            }
            HStack {
                Spacer()
                if job.finished {
                    Button("Close") { store.packaging = nil }
                    if succeeded, let r = job.result {
                        Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([r]) }
                        if job.target == .arm {
                            Button("Open") { NSWorkspace.shared.open(r); store.packaging = nil }.buttonStyle(.borderedProminent).tint(accent)
                        }
                    }
                } else {
                    ProgressView().controlSize(.small)
                }
            }
        }
        .padding(24).frame(width: 440)
        .preferredColorScheme(.dark)
    }

    var heading: String {
        if job.problem != nil { return "Couldn't package \(job.title)" }
        return succeeded ? "\(job.title) is ready" : "Packaging \(job.title)"
    }

    func row(_ i: Int, _ s: (id: String, title: String, icon: String)) -> some View {
        let finished = succeeded || i < current
        let active = i == current && !job.finished
        let failed = i == current && job.problem != nil
        return HStack(alignment: .top, spacing: 12) {
            VStack(spacing: 0) {
                ZStack {
                    Circle().fill(failed ? Color.orange : finished ? accent.opacity(0.25) : active ? accent : Color.white.opacity(0.06)).frame(width: 34, height: 34)
                    Image(systemName: failed ? "xmark" : finished ? "checkmark" : s.icon).font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(failed || active ? .white : finished ? accent : .secondary)
                }
                if i < steps.count - 1 { Rectangle().fill(.white.opacity(finished ? 0.3 : 0.08)).frame(width: 2, height: 18) }
            }
            VStack(alignment: .leading, spacing: 5) {
                Text(s.title).font(.system(size: 14, weight: active ? .semibold : .regular)).foregroundStyle(active || finished ? .primary : .secondary)
                if active && job.total > 0 {
                    ProgressView(value: Double(job.done), total: Double(job.total))
                    Text("\(job.done) of \(job.total) files").font(.caption).foregroundStyle(.secondary).monospacedDigit()
                }
            }
            .padding(.top, 7)
        }
    }
}

struct OptionsCard: View {
    @EnvironmentObject var store: Store
    let game: Game

    func binding<T>(_ path: WritableKeyPath<GameOptions, T>) -> Binding<T> {
        Binding(get: { game.options[keyPath: path] }, set: {
            var o = game.options
            o[keyPath: path] = $0
            store.setOptions(game, o)
        })
    }

    var note: String {
        if game.options.picture < 2 { return "" }
        if game.mode7Percent >= 1 {
            return String(format: "Only Mode 7 scenes really get wider: %.0f%% of frames measured. The rest show black side bars.", game.mode7Percent)
        }
        return "This game draws no Mode 7 scenes in the frames measured, so it stays its original width with black side bars."
    }

    func set(_ change: (inout GameOptions) -> Void) {
        var o = game.options
        change(&o)
        store.setOptions(game, o)
    }

    var pictures: [(Int, String, CGFloat)] {
        game.isN64 ? [(0, "Original 4:3", 4.0 / 3), (2, "16:9", 16.0 / 9), (3, "Ultrawide", 21.0 / 9)]
                   : [(0, "Original", 8.0 / 7), (1, "4:3 TV", 4.0 / 3), (2, "16:9", 16.0 / 9), (3, "Ultrawide", 21.0 / 9)]
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Options for this game").font(.headline)
            OptionGroup(title: "Picture") {
                ForEach(pictures, id: \.0) { tag, name, ratio in
                    OptionTile(name: name, selected: game.options.picture == tag) { set { $0.picture = tag } } content: {
                        RoundedRectangle(cornerRadius: 3).fill(LinearGradient(colors: [accent, .purple], startPoint: .topLeading, endPoint: .bottomTrailing))
                            .aspectRatio(ratio, contentMode: .fit).frame(maxWidth: 76, maxHeight: 44)
                    }
                }
            }
            if !game.isN64 && !note.isEmpty { Text(note).font(.caption).foregroundStyle(.secondary) }
            OptionGroup(title: "Filter") {
                ForEach(Array(["Sharp", "Smooth", "Scanlines", "CRT"].enumerated()), id: \.offset) { i, name in
                    OptionTile(name: name, selected: game.options.filter == i) { set { $0.filter = i } } content: { FilterSample(filter: i) }
                }
            }
            OptionGroup(title: "Input lag") {
                ForEach(Array(["Original", "1 frame less", "2 frames less"].enumerated()), id: \.offset) { i, name in
                    OptionTile(name: name, selected: game.options.runahead == i) { set { $0.runahead = i } } content: { LagBar(saved: i) }
                }
            }
            VStack(alignment: .leading, spacing: 6) {
                Text("Speed").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                HStack(spacing: 10) {
                    Image(systemName: "tortoise.fill").foregroundStyle(.secondary)
                    Slider(value: Binding(get: { Double(game.options.speed) }, set: { v in set { $0.speed = Int(v) } }), in: 50...200, step: 10)
                    Image(systemName: "hare.fill").foregroundStyle(.secondary)
                    Text("\(game.options.speed)%").monospacedDigit().frame(width: 44, alignment: .trailing)
                }
            }
            HStack(alignment: .top, spacing: 10) {
                SwitchTile(icon: "record.circle", color: .red, name: "Record new code", detail: "Finds code to compile as you play. F8 in game.",
                           isOn: game.options.record != 0) { v in set { $0.record = v ? 1 : 0 } }
                if !game.isN64 {
                    SwitchTile(icon: "sparkles", color: accent, name: "No sprite flicker", detail: "Draws past the console's sprite limit.",
                               isOn: game.options.sprites != 0) { v in set { $0.sprites = v ? 1 : 0 } }
                }
            }
            .fixedSize(horizontal: false, vertical: true)
            if !game.isN64 { Text("Applies on this Mac. The iMac keeps its own settings.").font(.caption).foregroundStyle(.secondary) }
        }
        .padding(14).background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
    }
}

struct OptionGroup<Content: View>: View {
    let title: String
    @ViewBuilder let content: Content
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.caption.weight(.semibold)).foregroundStyle(.secondary)
            HStack(spacing: 8) { content }
        }
    }
}

/// One choice, shown as a picture of what it does.
struct OptionTile<Content: View>: View {
    let name: String
    let selected: Bool
    let action: () -> Void
    @ViewBuilder let content: Content
    var body: some View {
        Button(action: action) {
            VStack(spacing: 7) {
                content.frame(maxWidth: .infinity).frame(height: 48)
                Text(name).font(.system(size: 12, weight: selected ? .semibold : .regular)).lineLimit(1)
            }
            .padding(9).frame(maxWidth: .infinity)
            .background(selected ? accent.opacity(0.18) : Color.white.opacity(0.04), in: RoundedRectangle(cornerRadius: 10))
            .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(selected ? accent : .white.opacity(0.08), lineWidth: selected ? 2 : 1))
            .contentShape(Rectangle())
        }.buttonStyle(.plain)
    }
}

/// The same little pixel picture through each video filter.
struct FilterSample: View {
    let filter: Int
    static let art = ["..RRRR..", ".RRRRRR.", "RWRRRRWR", "RRRRRRRR", "..YYYY..", ".YKYYKY.", ".YYYYYY.", "..YYYY.."]
    var body: some View {
        Canvas { ctx, size in
            let s = floor(min(size.width, size.height) / 8), x0 = (size.width - s * 8) / 2, y0 = (size.height - s * 8) / 2
            var c = ctx
            if filter == 1 || filter == 3 { c.addFilter(.blur(radius: filter == 1 ? 1.4 : 0.8)) }
            for (y, row) in Self.art.enumerated() {
                for (x, p) in row.enumerated() {
                    let color: Color = p == "R" ? .red : p == "W" ? .white : p == "Y" ? .yellow : p == "K" ? .black : Color(white: 0.16)
                    c.fill(Path(CGRect(x: x0 + Double(x) * s, y: y0 + Double(y) * s, width: s + 0.5, height: s + 0.5)), with: .color(color))
                }
            }
            if filter >= 2 {
                var y = y0
                while y < y0 + s * 8 {
                    ctx.fill(Path(CGRect(x: x0, y: y + s * 0.55, width: s * 8, height: s * 0.4)), with: .color(.black.opacity(filter == 2 ? 0.6 : 0.4)))
                    y += s
                }
            }
            if filter == 3 {
                ctx.stroke(Path(roundedRect: CGRect(x: x0 - 1, y: y0 - 1, width: s * 8 + 2, height: s * 8 + 2), cornerRadius: 6), with: .color(.black), lineWidth: 3)
            }
        }
    }
}

/// Frames between pressing a button and seeing it happen.
struct LagBar: View {
    let saved: Int
    var body: some View {
        HStack(spacing: 3) {
            Image(systemName: "gamecontroller.fill").font(.system(size: 13)).foregroundStyle(.secondary)
            ForEach(0..<(3 - saved), id: \.self) { _ in RoundedRectangle(cornerRadius: 2).fill(Color.orange).frame(width: 12, height: 16) }
            ForEach(0..<saved, id: \.self) { _ in RoundedRectangle(cornerRadius: 2).strokeBorder(.white.opacity(0.18)).frame(width: 12, height: 16) }
            Image(systemName: "display").font(.system(size: 13)).foregroundStyle(.secondary)
        }
    }
}

struct SwitchTile: View {
    let icon: String
    let color: Color
    let name: String
    let detail: String
    let isOn: Bool
    let set: (Bool) -> Void
    var body: some View {
        Button { set(!isOn) } label: {
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: icon).font(.system(size: 18)).foregroundStyle(isOn ? color : .secondary).frame(width: 24)
                VStack(alignment: .leading, spacing: 2) {
                    Text(name).font(.system(size: 13, weight: .semibold))
                    Text(detail).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 0)
                Image(systemName: isOn ? "checkmark.circle.fill" : "circle").foregroundStyle(isOn ? accent : .secondary)
            }
            .padding(10).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .background(isOn ? color.opacity(0.12) : Color.white.opacity(0.04), in: RoundedRectangle(cornerRadius: 10))
            .contentShape(Rectangle())
        }.buttonStyle(.plain)
    }
}

struct Stat: View {
    let value: String
    let label: String
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(value).font(.system(size: 24, weight: .semibold, design: .rounded)).monospacedDigit()
            Text(label).font(.caption).foregroundStyle(.secondary)
        }
        .padding(14).frame(maxWidth: .infinity, alignment: .leading)
        .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
    }
}

struct StepStrip: View {
    let game: Game
    var steps: [(id: String, title: String, icon: String)] { game.isPSP ? pspBuildSteps : game.isN64 ? buildSteps.filter { $0.id != "os9" } : buildSteps }
    var body: some View {
        HStack(spacing: 0) {
            ForEach(Array(steps.enumerated()), id: \.offset) { i, s in
                let finished = game.done.contains(s.id) || (game.status == "ready" && !game.building && !game.queued && game.problem == nil)
                let active = game.step == s.id && game.building
                VStack(spacing: 6) {
                    ZStack {
                        Circle().fill(finished ? accent.opacity(0.25) : active ? accent : Color.white.opacity(0.06)).frame(width: 38, height: 38)
                        Image(systemName: finished ? "checkmark" : s.icon).font(.system(size: 14, weight: .semibold))
                            .foregroundStyle(finished ? accent : active ? .white : .secondary)
                    }
                    Text(s.title).font(.caption2).foregroundStyle(active ? .primary : .secondary)
                }
                .frame(maxWidth: .infinity)
                if i < steps.count - 1 {
                    Rectangle().fill(.white.opacity(finished ? 0.3 : 0.08)).frame(height: 1).offset(y: -9).frame(maxWidth: 26)
                }
            }
        }
    }
}
