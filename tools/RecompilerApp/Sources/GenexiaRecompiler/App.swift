import SwiftUI
import UniformTypeIdentifiers

let accent = Color(red: 0.36, green: 0.62, blue: 1.0)
let romExtensions: Set<String> = ["sfc", "smc", "md", "gen", "bin", "z64", "n64", "v64"]

@main
struct RecompilerApp: App {
    @StateObject private var store = Store()
    init() { NSApplication.shared.setActivationPolicy(.regular) }
    var body: some Scene {
        WindowGroup("Genexia Recompiler") {
            MainView().environmentObject(store)
                .frame(minWidth: 860, minHeight: 560)
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
                if let g = store.games.first(where: { $0.id == store.selection }) {
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
            .padding(.horizontal, 18).padding(.top, 38).padding(.bottom, 14)
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
                Text(big ? "Drop a ROM here" : "Drop or choose a ROM").font(big ? .title2.weight(.medium) : .caption)
                if big { Text("SNES today. Genesis and Nintendo 64 are recognized and coming next.").font(.callout).foregroundStyle(.secondary) }
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

struct ConsoleBadge: View {
    let console: String
    var color: Color { console == "SNES" ? .purple : console == "Genesis" ? .blue : console.isEmpty ? .gray : .green }
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
        Button { store.selection = game.id } label: {
            HStack(spacing: 10) {
                Group {
                    if game.building { ProgressView().controlSize(.small) }
                    else if game.queued { Image(systemName: "clock").foregroundStyle(.secondary) }
                    else if game.problem != nil { Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange) }
                    else { Image(systemName: "checkmark.circle.fill").foregroundStyle(.green) }
                }.frame(width: 20)
                VStack(alignment: .leading, spacing: 3) {
                    Text(game.title.capitalized).font(.system(size: 13, weight: .medium)).lineLimit(1)
                    ConsoleBadge(console: game.console)
                }
                Spacer()
            }
            .padding(9)
            .background(store.selection == game.id ? accent.opacity(0.2) : .clear, in: RoundedRectangle(cornerRadius: 9))
            .contentShape(Rectangle())
        }.buttonStyle(.plain)
    }
}

struct GameDetail: View {
    @EnvironmentObject var store: Store
    let game: Game

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            HStack(alignment: .firstTextBaseline, spacing: 10) {
                Text(game.title.capitalized).font(.system(size: 28, weight: .bold, design: .rounded)).lineLimit(1)
                ConsoleBadge(console: game.console)
                Spacer()
            }
            StepStrip(game: game)
            if let p = game.problem {
                Label(p, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                    .padding(12).frame(maxWidth: .infinity, alignment: .leading)
                    .background(Color.orange.opacity(0.1), in: RoundedRectangle(cornerRadius: 10))
            }
            HStack(spacing: 12) {
                Stat(value: game.instructions.formatted(), label: "instructions compiled")
                Stat(value: String(format: "%.2f%%", game.interpreterPercent), label: "left to the interpreter")
                Stat(value: game.checkedBlocks == 0 ? "–" : compact(game.checkedBlocks), label: game.mismatches == 0 ? "blocks verified" : "\(game.mismatches) mismatches")
            }
            if !game.log.isEmpty {
                ScrollView {
                    Text(game.log.joined(separator: "\n")).font(.system(size: 11, design: .monospaced)).foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled)
                }
                .padding(10).background(Color.black.opacity(0.25), in: RoundedRectangle(cornerRadius: 10))
            } else { Spacer() }
            HStack {
                Button { store.play(game) } label: { Label("Play", systemImage: "play.fill") }
                    .buttonStyle(.borderedProminent).tint(accent).disabled(game.building || game.problem != nil)
                Button { store.sendToIMac(game) } label: { Label("Send to iMac", systemImage: "desktopcomputer") }
                    .disabled(game.os9Library == nil || game.building)
                Button { store.rebuild(game) } label: { Label("Grow again", systemImage: "arrow.triangle.2.circlepath") }
                    .disabled(game.building || game.queued)
                Spacer()
                Button { store.reveal(game) } label: { Image(systemName: "folder") }.help("Show the library in Finder")
            }
        }
        .padding(28).padding(.top, 8)
    }

    func compact(_ n: Int) -> String {
        n >= 1_000_000 ? String(format: "%.1fM", Double(n) / 1_000_000) : n.formatted()
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
    var body: some View {
        HStack(spacing: 0) {
            ForEach(Array(buildSteps.enumerated()), id: \.offset) { i, s in
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
                if i < buildSteps.count - 1 {
                    Rectangle().fill(.white.opacity(finished ? 0.3 : 0.08)).frame(height: 1).offset(y: -9).frame(maxWidth: 26)
                }
            }
        }
    }
}
