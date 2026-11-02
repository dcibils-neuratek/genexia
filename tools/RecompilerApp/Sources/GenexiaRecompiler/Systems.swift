import SwiftUI

/// What Genexia can run, chip by chip.
enum ChipState {
    case native, emulated, missing
    var color: Color { self == .native ? .green : self == .emulated ? accent : .gray }
    var label: String { self == .native ? "Recompiled to native code" : self == .emulated ? "Emulated" : "Not yet" }
}

struct Chip: Identifiable {
    let name: String
    let job: String
    var clock = ""
    var state = ChipState.emulated
    var id: String { name }
}

struct Ability: Identifiable {
    let name: String
    let detail: String
    let yes: Bool
    var id: String { name }
}

struct System: Identifiable {
    let name: String
    let year: String
    let abilities: [Ability]
    let chips: [Chip]
    var id: String { name }
}

let systems = [
    System(name: "Genesis", year: "1988", abilities: [
        Ability(name: "Emulate", detail: "Any ROM", yes: true),
        Ability(name: "Recompile", detail: "Games built into Genexia, not from this app yet", yes: false),
        Ability(name: "Package", detail: "Not yet", yes: false)
    ], chips: [
        Chip(name: "68000", job: "Main CPU", clock: "7.67 MHz", state: .native),
        Chip(name: "Z80", job: "Sound CPU", clock: "3.58 MHz"),
        Chip(name: "VDP", job: "Picture"),
        Chip(name: "YM2612", job: "FM synth"),
        Chip(name: "SN76489", job: "Square waves")
    ]),
    System(name: "SNES", year: "1990", abilities: [
        Ability(name: "Emulate", detail: "Any ROM without an unsupported cartridge chip", yes: true),
        Ability(name: "Recompile", detail: "From this app", yes: true),
        Ability(name: "Package", detail: "Mac ARM, Intel, PowerPC", yes: true)
    ], chips: [
        Chip(name: "65C816", job: "Main CPU", clock: "3.58 MHz", state: .native),
        Chip(name: "SPC700", job: "Sound CPU", clock: "1.02 MHz"),
        Chip(name: "S-DSP", job: "Sound"),
        Chip(name: "PPU", job: "Picture and Mode 7"),
        Chip(name: "DSP-1", job: "Cartridge maths"),
        Chip(name: "Super FX, SA-1", job: "Other cartridge chips", state: .missing)
    ]),
    System(name: "Nintendo 64", year: "1996", abilities: [
        Ability(name: "Emulate", detail: "Only Super Mario 64 tested so far", yes: true),
        Ability(name: "Recompile", detail: "From this app", yes: true),
        Ability(name: "Package", detail: "Mac ARM, Intel", yes: true)
    ], chips: [
        Chip(name: "R4300i", job: "Main CPU and FPU", clock: "93.75 MHz", state: .native),
        Chip(name: "RSP", job: "Runs the game's microcode", clock: "62.5 MHz"),
        Chip(name: "RDP", job: "Draws the picture", clock: "62.5 MHz"),
        Chip(name: "EEPROM", job: "Cartridge saves"),
        Chip(name: "SRAM, Flash", job: "Other cartridge saves", state: .missing)
    ])
]

struct Systems: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                VStack(alignment: .leading, spacing: 6) {
                    Text("Systems").font(.system(size: 28, weight: .bold, design: .rounded))
                    Text("The consoles Genexia runs, and what happens to each chip inside them.")
                        .font(.title3).foregroundStyle(.secondary)
                }
                HStack(spacing: 16) {
                    ForEach([ChipState.native, .emulated, .missing], id: \.self) { s in
                        HStack(spacing: 6) {
                            RoundedRectangle(cornerRadius: 2).fill(s.color).frame(width: 12, height: 12)
                            Text(s.label).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
                ForEach(systems) { SystemCard(system: $0) }
            }
            .padding(.horizontal, 28).padding(.top, 34).padding(.bottom, 24)
        }
    }
}

struct SystemCard: View {
    let system: System
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 14) {
                Cartridge(console: system.name).frame(width: 84, height: 84)
                VStack(alignment: .leading, spacing: 2) {
                    Text(system.name).font(.system(size: 20, weight: .bold, design: .rounded))
                    Text(system.year).font(.callout).foregroundStyle(.secondary)
                }
            }
            HStack(alignment: .top, spacing: 8) {
                ForEach(system.abilities) { AbilityTile(ability: $0) }
            }
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 132), spacing: 14)], spacing: 14) {
                ForEach(system.chips) { ChipTile(chip: $0) }
            }
            .padding(14)
            .background(Color(red: 0.05, green: 0.14, blue: 0.11), in: RoundedRectangle(cornerRadius: 10))
        }
        .padding(16)
        .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 14))
    }
}

struct AbilityTile: View {
    let ability: Ability
    var body: some View {
        HStack(alignment: .top, spacing: 7) {
            Image(systemName: ability.yes ? "checkmark.circle.fill" : "circle.dashed")
                .foregroundStyle(ability.yes ? .green : .secondary)
            VStack(alignment: .leading, spacing: 2) {
                Text(ability.name).font(.system(size: 12, weight: .semibold))
                Text(ability.detail).font(.caption2).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// A chip on the board: a black package with its pins, lit by what Genexia does with it.
struct ChipTile: View {
    let chip: Chip
    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(chip.name).font(.system(size: 13, weight: .bold, design: .monospaced)).lineLimit(1).minimumScaleFactor(0.7)
                .foregroundStyle(chip.state == .missing ? .secondary : chip.state.color)
            Text(chip.job).font(.caption2).foregroundStyle(.secondary).lineLimit(2, reservesSpace: true)
            Text(chip.clock.isEmpty ? " " : chip.clock).font(.system(size: 10, design: .monospaced)).foregroundStyle(.tertiary)
        }
        .padding(10).frame(maxWidth: .infinity, alignment: .leading)
        .background(chip.state == .missing ? Color.clear : Color(white: 0.08), in: RoundedRectangle(cornerRadius: 4))
        .overlay {
            RoundedRectangle(cornerRadius: 4).strokeBorder(chip.state.color.opacity(chip.state == .missing ? 0.5 : 0.8),
                style: StrokeStyle(lineWidth: 1, dash: chip.state == .missing ? [4, 3] : []))
        }
        .background { if chip.state != .missing { Pins() } }
    }
}

struct Pins: View {
    var body: some View {
        Canvas { ctx, size in
            var x = 7.0
            while x < size.width - 6 {
                ctx.fill(Path(CGRect(x: x, y: 0, width: 3, height: size.height)), with: .color(Color(white: 0.62)))
                x += 9
            }
        }
        .padding(.vertical, -4)
    }
}
