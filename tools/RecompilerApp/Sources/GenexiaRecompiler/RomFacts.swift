import SwiftUI
import zlib

/// What a ROM's own header says about the cartridge.
struct RomFact: Identifiable {
    let label: String
    let value: String
    var warning = false
    var id: String { label }
}

func romFacts(path: String, console: String) -> [RomFact] {
    guard let file = FileHandle(forReadingAtPath: path), let size = try? file.seekToEnd() else { return [] }
    try? file.seek(toOffset: 0)
    let head = [UInt8](file.readData(ofLength: 0x10400))
    let mb = String(format: "%.3g MB", Double(size) / 1_048_576)
    if console == "SNES" { return snesFacts(head, size: Int(size), mb: mb) }
    if console == "Nintendo 64" { return n64Facts(head, mb: mb) }
    if console == "PSP" { return [RomFact(label: "Disc", value: mb)] }
    return [RomFact(label: "ROM", value: mb)]
}

private func snesFacts(_ rom: [UInt8], size: Int, mb: String) -> [RomFact] {
    let skip = size % 1024 == 512 ? 512 : 0
    let bases = [0x7FC0, 0xFFC0].map { $0 + skip }.filter { $0 + 0x20 <= rom.count }
    func valid(_ b: Int) -> Bool { Int(rom[b + 0x1C]) + Int(rom[b + 0x1D]) << 8 + Int(rom[b + 0x1E]) + Int(rom[b + 0x1F]) << 8 == 0xFFFF }
    guard let b = bases.first(where: valid) ?? bases.first else { return [RomFact(label: "ROM", value: mb)] }
    let map = rom[b + 0x15], type = rom[b + 0x16], ram = rom[b + 0x18]
    let layout = (map & 15 == 1 ? "HiROM" : map & 15 == 5 ? "ExHiROM" : "LoROM") + (map & 0x10 != 0 ? ", FastROM" : ", SlowROM")
    var chip = "None", supported = true
    if type & 15 >= 3 {
        switch type >> 4 {
        case 0: chip = "DSP-1"
        case 1: chip = "Super FX"; supported = false
        case 2: chip = "OBC-1"; supported = false
        case 3: chip = "SA-1"; supported = false
        case 4: chip = "S-DD1"; supported = false
        case 5: chip = "S-RTC"; supported = false
        default: chip = "Special chip"; supported = false
        }
    }
    let battery = [2, 5, 6].contains(type & 15)
    let save = !battery ? "None" : ram > 0 ? "Battery, \(1 << Int(ram)) KB" : "Battery"
    let regions = [0: "Japan", 1: "USA", 2: "Europe"]
    return [
        RomFact(label: "ROM", value: "\(mb), \(size * 8 / 1_048_576) Mbit"),
        RomFact(label: "Layout", value: layout),
        RomFact(label: "Cartridge chip", value: supported ? chip : chip + ", not supported yet", warning: !supported),
        RomFact(label: "Save", value: save),
        RomFact(label: "Region", value: regions[Int(rom[b + 0x19])] ?? "Other")
    ]
}

private func n64Facts(_ rom: [UInt8], mb: String) -> [RomFact] {
    guard rom.count >= 0x1000, rom[0] == 0x80 else { return [RomFact(label: "ROM", value: mb)] }
    let boot = rom[0x40..<0x1000].withUnsafeBufferPointer { crc32(0, $0.baseAddress, UInt32($0.count)) }
    let cic: [UInt: String] = [0x6170A4A1: "CIC-6101", 0x90BB6CB5: "CIC-6102", 0x0B050EE0: "CIC-6103", 0x98BC2C86: "CIC-6105", 0xACC8580A: "CIC-6106"]
    let regions: [UInt8: String] = [0x45: "USA", 0x4A: "Japan", 0x50: "Europe"]
    let name = cic[boot] ?? "Unknown"
    return [
        RomFact(label: "ROM", value: mb),
        RomFact(label: "Boot chip", value: name, warning: name != "CIC-6102"),
        RomFact(label: "Region", value: regions[rom[0x3E]] ?? "Other")
    ]
}

struct RomFactsRow: View {
    let facts: [RomFact]
    var body: some View {
        HStack(alignment: .top, spacing: 18) {
            ForEach(facts) { f in
                VStack(alignment: .leading, spacing: 1) {
                    Text(f.label).font(.caption2).foregroundStyle(.secondary)
                    Text(f.value).font(.system(size: 12, weight: .semibold, design: .rounded)).foregroundStyle(f.warning ? .orange : .primary)
                }
            }
            Spacer(minLength: 0)
        }
    }
}
