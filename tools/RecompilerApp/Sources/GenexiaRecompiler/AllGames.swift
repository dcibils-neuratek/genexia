import SwiftUI
import UniformTypeIdentifiers

/// The shelf: every game as a cartridge you own, to pick up and use.
struct AllGames: View {
    @EnvironmentObject var store: Store
    @State private var console: String?          // nil shows every system
    @State private var at = 0
    @State private var drag: CGFloat = 0
    @State private var monitor: Any?
    @State private var scrolled: CGFloat = 0
    @State private var choosingArt = false

    var shown: [Game] { store.games.filter { console == nil || $0.console == console } }
    var consoles: [String] { Array(Set(store.games.map(\.console))).filter { !$0.isEmpty }.sorted() }
    var current: Game? { shown.indices.contains(at) ? shown[at] : nil }

    var body: some View {
        ZStack {
            if let g = current, let image = store.art[g.id] ?? store.shots[g.id] {
                Image(nsImage: image).resizable().aspectRatio(contentMode: .fill)
                    .frame(minWidth: 0, maxWidth: .infinity, minHeight: 0, maxHeight: .infinity)
                    .blur(radius: 70).opacity(0.3).clipped().id(g.id).transition(.opacity)
            }
            VStack(spacing: 0) {
                header.padding(.horizontal, 28).padding(.top, 34)
                if shown.isEmpty {
                    Spacer(); DropHint(big: true); Spacer()
                } else {
                    GeometryReader { geo in shelf(geo.size) }
                    if let g = current { panel(g).padding(.horizontal, 28).padding(.bottom, 24) }
                }
            }
        }
        .animation(.easeInOut(duration: 0.35), value: current?.id)
        .onAppear { if let i = shown.firstIndex(where: { $0.id == store.selection }) { at = i } else { pick(shown.count / 2) }; listen() }
        .onDisappear { if let monitor { NSEvent.removeMonitor(monitor) }; monitor = nil }
        .onChange(of: console) { pick(shown.firstIndex { $0.id == store.selection } ?? 0) }
        .onChange(of: store.games.count) { pick(min(at, max(0, shown.count - 1))) }
        .fileImporter(isPresented: $choosingArt, allowedContentTypes: [.image]) { r in
            if let u = try? r.get(), let g = current { store.setArt(g, from: u) }
        }
    }

    var header: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Text("All Games").font(.system(size: 28, weight: .bold, design: .rounded))
            Text("\(shown.count) \(shown.count == 1 ? "cartridge" : "cartridges")").font(.callout).foregroundStyle(.secondary)
            Spacer()
            if consoles.count > 1 {
                HStack(spacing: 4) {
                    chip("All", nil)
                    ForEach(consoles, id: \.self) { chip($0 == "Nintendo 64" ? "N64" : $0, $0) }
                }
            }
        }
    }

    func chip(_ title: String, _ value: String?) -> some View {
        Button { console = value } label: {
            Text(title).font(.system(size: 12, weight: .semibold, design: .rounded)).padding(.horizontal, 11).padding(.vertical, 5)
                .background(console == value ? accent.opacity(0.35) : Color.white.opacity(0.07), in: Capsule())
        }.buttonStyle(.plain)
    }

    func shelf(_ size: CGSize) -> some View {
        let side = min(size.height * 0.86, size.width * 0.58)
        let position = CGFloat(at) - drag / (side * 0.7)
        return ZStack {
            ForEach(Array(shown.enumerated()), id: \.element.id) { i, g in
                let d = CGFloat(i) - position
                if abs(d) < 5 {
                    let near = min(abs(d), 1), far = max(abs(d) - 1, 0)
                    cartridge(g, side, i == at)
                        .scaleEffect(1 - 0.34 * near - 0.06 * min(far, 3))
                        .rotation3DEffect(.degrees(Double(max(-1, min(1, d))) * -56), axis: (x: 0, y: 1, z: 0), perspective: 0.45)
                        .offset(x: (d < 0 ? -1 : 1) * (near * side * 0.72 + far * side * 0.3))
                        .opacity(1 - 0.16 * Double(abs(d)))
                        .zIndex(-Double(abs(d)))
                        .onTapGesture { if i == at { store.selection == g.id ? open(g, .compile) : pick(i) } else { pick(i) } }
                }
            }
        }
        .frame(width: size.width, height: size.height)
        .contentShape(Rectangle())
        .clipped()
        .gesture(DragGesture(minimumDistance: 8)
            .onChanged { drag = $0.translation.width }
            .onEnded { v in
                let move = Int((v.predictedEndTranslation.width / (side * 0.7)).rounded())
                drag = 0
                pick(max(0, min(shown.count - 1, at - move)))
            })
        .animation(.spring(response: 0.42, dampingFraction: 0.84), value: at)
        .animation(.interactiveSpring(), value: drag)
    }

    func cartridge(_ g: Game, _ side: CGFloat, _ front: Bool) -> some View {
        ZStack {
            store.cartridge(g, big: true).frame(width: side, height: side)      // its reflection on the shelf
                .scaleEffect(y: -1).opacity(0.18)
                .mask(LinearGradient(stops: [.init(color: .white, location: 0), .init(color: .clear, location: 0.3)], startPoint: .top, endPoint: .bottom).padding(.top, side * 0.16))
                .offset(y: side * 0.68)
                .allowsHitTesting(false)
            store.cartridge(g, big: true).frame(width: side, height: side)
                .shadow(color: .black.opacity(0.55), radius: 22, y: 14)
        }
        .contextMenu {
            Button("Choose Label Art…") { pick(shown.firstIndex(of: g) ?? at); choosingArt = true }
            if store.art[g.id] != nil { Button("Use a Picture of the Game") { store.clearArt(g) } }
            Divider()
            Button("Remove Game…", role: .destructive) { store.removing = g }.disabled(g.building || g.queued)
        }
    }

    func panel(_ g: Game) -> some View {
        HStack(spacing: 18) {
            if let shot = store.shots[g.id] {
                Image(nsImage: shot).interpolation(.none).resizable().aspectRatio(contentMode: .fit)
                    .frame(maxHeight: 84).layoutPriority(-1).clipShape(RoundedRectangle(cornerRadius: 6))
                    .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(.white.opacity(0.14)))
            }
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 8) {
                    Text(g.title.capitalized).font(.system(size: 22, weight: .bold, design: .rounded)).lineLimit(1)
                    ConsoleBadge(console: g.console)
                }
                Text(state(g)).font(.callout).foregroundStyle(.secondary).lineLimit(2).fixedSize(horizontal: false, vertical: true)
                HStack(spacing: 8) {
                    Button { g.encrypted ? store.dumpWithPPSSPP(g) : store.play(g) } label: {
                        Label(g.encrypted ? "Dump with PPSSPP" : g.isPSP && g.instructions == 0 ? "Compile" : "Play", systemImage: g.encrypted ? "arrow.down.doc" : "play.fill").labelStyle(.titleAndIcon).font(.system(size: 14, weight: .semibold)).padding(.horizontal, 10).padding(.vertical, 2)
                    }
                    .buttonStyle(.borderedProminent).tint(accent).disabled(g.building || (g.problem != nil && !g.encrypted))
                    Button { open(g, .compile) } label: { Label(g.isPSP && g.instructions == 0 ? "Details" : "Compile", systemImage: "cpu").labelStyle(.titleAndIcon) }
                    if !g.isPSP {
                        Menu {
                            ForEach(PackageTarget.allCases) { t in
                                Button(t.title) { store.package(g, for: t) }.disabled(!t.available(g))
                            }
                        } label: { Label("Package", systemImage: "shippingbox") }
                            .fixedSize()
                            .disabled(g.building || g.queued || store.packaging != nil)
                    }
                    Button { store.removing = g } label: { Image(systemName: "trash") }
                        .disabled(g.building || g.queued).help("Remove this game from Genexia")
                }
                .fixedSize()
            }
            Spacer(minLength: 0)
            if !g.isPSP { NativeRing(percent: g.nativePercent) }
        }
        .padding(14)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 14))
    }

    func state(_ g: Game) -> String {
        if g.building { return g.detail.isEmpty ? "Building…" : g.detail }
        if g.queued { return "Waiting for its turn to build" }
        if let p = g.problem { return p }
        if g.instructions == 0 { return "Not compiled yet" }
        let facts = store.facts(for: g).prefix(2).map(\.value)
        if g.isPSP { return (facts + ["\(g.instructions.formatted()) instructions compiled"]).joined(separator: " · ") }
        return (facts + [g.nativePercent >= 99.95 ? "Fully native" : "Runs native, more to find by playing"]).joined(separator: " · ")
    }

    func pick(_ i: Int) {
        guard shown.indices.contains(i) else { return }
        at = i
        store.selection = shown[i].id
    }

    func open(_ g: Game, _ tab: GameTab) {
        store.selection = g.id
        store.tab = tab
        store.info = nil
    }

    /// Arrow keys, Return and the trackpad, while nothing else wants them.
    func listen() {
        guard monitor == nil else { return }
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .scrollWheel]) { e in
            guard store.info == .library, store.removing == nil, store.packaging == nil, !choosingArt,
                  e.window?.isKeyWindow == true, e.window?.firstResponder is NSTextView == false else { return e }
            if e.type == .keyDown {
                switch e.keyCode {
                case 123: pick(at - 1)
                case 124: pick(at + 1)
                case 36: if let g = current, !g.building, g.problem == nil { store.play(g) }
                default: return e
                }
                return nil
            }
            guard e.locationInWindow.x > 280 else { return e }      // the sidebar scrolls itself
            let delta = abs(e.scrollingDeltaX) > abs(e.scrollingDeltaY) ? e.scrollingDeltaX : e.scrollingDeltaY
            if !e.hasPreciseScrollingDeltas {       // a mouse wheel: one game a notch
                if delta != 0 { pick(at + (delta < 0 ? 1 : -1)) }
                return nil
            }
            if e.phase == .began { scrolled = 0 }
            scrolled += delta
            if abs(scrolled) > 44 {
                pick(at + (scrolled < 0 ? 1 : -1))
                scrolled = 0
            }
            return nil
        }
    }
}
