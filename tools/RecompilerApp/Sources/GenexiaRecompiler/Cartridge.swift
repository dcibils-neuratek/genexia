import SwiftUI

/// A drawing of the console's cartridge, no logos, with the game's art on its label.
struct Cartridge: View {
    let console: String
    var label: NSImage? = nil
    var title: String? = nil        // printed on the label when the art doesn't carry a name
    var body: some View {
        Canvas { ctx, size in
            let u = min(size.width, size.height) / 24
            ctx.translateBy(x: (size.width - 24 * u) / 2, y: (size.height - 24 * u) / 2)
            ctx.scaleBy(x: u, y: u)
            switch console {
            case "SNES": snes(ctx)
            case "Nintendo 64": n64(ctx)
            case "Genesis": genesis(ctx)
            case "PSP": umd(ctx)
            default: plain(ctx)
            }
        }
        .aspectRatio(1, contentMode: .fit)
    }

    /// The game's art filling a shape, or the console's colours when there is none.
    func printed(_ c: GraphicsContext, _ r: CGRect, _ top: Color, _ bottom: Color) {
        if let label, label.size.width > 0, label.size.height > 0 {
            let k = max(r.width / label.size.width, r.height / label.size.height)
            let w = label.size.width * k, h = label.size.height * k
            c.draw(c.resolve(Image(nsImage: label)), in: CGRect(x: r.midX - w / 2, y: r.midY - h / 2, width: w, height: h))
        } else {
            c.fill(Path(r), with: .linearGradient(Gradient(colors: [top, bottom]),
                startPoint: CGPoint(x: r.minX, y: r.minY), endPoint: CGPoint(x: r.maxX, y: r.maxY)))
        }
    }

    func sticker(_ ctx: GraphicsContext, _ r: CGRect, _ top: Color, _ bottom: Color) {
        var c = ctx
        c.clip(to: Path(roundedRect: r, cornerRadius: 0.4))
        printed(c, r, top, bottom)
        if let title {
            let band = CGRect(x: r.minX, y: r.maxY - 2.6, width: r.width, height: 2.6)
            c.fill(Path(band), with: .linearGradient(Gradient(colors: [.clear, .black.opacity(0.8)]),
                startPoint: CGPoint(x: 0, y: band.minY), endPoint: CGPoint(x: 0, y: band.maxY)))
            c.draw(Text(title).font(.system(size: 0.95, weight: .heavy, design: .rounded)).foregroundColor(.white),
                   in: band.insetBy(dx: 0.4, dy: 0.5))
        }
        c.fill(Path(r), with: .linearGradient(Gradient(colors: [.white.opacity(0.16), .clear, .clear]),
            startPoint: CGPoint(x: r.minX, y: r.minY), endPoint: CGPoint(x: r.maxX, y: r.maxY)))
    }

    /// Moulded plastic: lit from above.
    func light(_ ctx: GraphicsContext, _ shape: Path) {
        ctx.fill(shape, with: .linearGradient(Gradient(colors: [.white.opacity(0.14), .clear, .black.opacity(0.28)]),
                                              startPoint: CGPoint(x: 0, y: 3), endPoint: CGPoint(x: 0, y: 21)))
    }

    /// North American cartridge: wide and flat-topped, with ribbed side wings around a raised centre.
    func snes(_ ctx: GraphicsContext) {
        let wing = Color(white: 0.56), edge = Color(white: 0.38), centre = Color(white: 0.64)
        var body = Path()
        body.move(to: CGPoint(x: 1.6, y: 4.5))
        body.addLine(to: CGPoint(x: 22.4, y: 4.5))
        body.addLine(to: CGPoint(x: 23, y: 5.1))
        body.addLine(to: CGPoint(x: 23, y: 18.4))
        body.addLine(to: CGPoint(x: 22.4, y: 19))
        body.addLine(to: CGPoint(x: 21, y: 19))
        body.addLine(to: CGPoint(x: 21, y: 19.8))
        body.addLine(to: CGPoint(x: 3, y: 19.8))
        body.addLine(to: CGPoint(x: 3, y: 19))
        body.addLine(to: CGPoint(x: 1.6, y: 19))
        body.addLine(to: CGPoint(x: 1, y: 18.4))
        body.addLine(to: CGPoint(x: 1, y: 5.1))
        body.closeSubpath()
        ctx.fill(body, with: .color(wing))
        light(ctx, body)
        for y in stride(from: 7.0, through: 16.0, by: 2.25) {
            ctx.fill(Path(CGRect(x: 1, y: y, width: 3.8, height: 0.45)), with: .color(edge))
            ctx.fill(Path(CGRect(x: 19.2, y: y, width: 3.8, height: 0.45)), with: .color(edge))
        }
        ctx.fill(Path(CGRect(x: 4.8, y: 4.5, width: 14.4, height: 15.3)), with: .color(centre))
        ctx.fill(Path(CGRect(x: 4.8, y: 4.5, width: 0.35, height: 15.3)), with: .color(edge))
        ctx.fill(Path(CGRect(x: 18.85, y: 4.5, width: 0.35, height: 15.3)), with: .color(edge))
        sticker(ctx, CGRect(x: 6, y: 6, width: 12, height: 10.6), Color(red: 0.3, green: 0.3, blue: 0.75), Color(red: 0.75, green: 0.35, blue: 0.5))
    }

    /// Arched top, flat bottom, a recessed panel between two grooved wings.
    func n64(_ ctx: GraphicsContext) {
        let shell = Color(white: 0.62), groove = Color(white: 0.4)
        var body = Path()
        body.move(to: CGPoint(x: 2.6, y: 20))
        body.addLine(to: CGPoint(x: 2.6, y: 18.8))
        body.addLine(to: CGPoint(x: 1, y: 18.8))
        body.addLine(to: CGPoint(x: 1, y: 8))
        body.addQuadCurve(to: CGPoint(x: 23, y: 8), control: CGPoint(x: 12, y: 0.8))
        body.addLine(to: CGPoint(x: 23, y: 18.8))
        body.addLine(to: CGPoint(x: 21.4, y: 18.8))
        body.addLine(to: CGPoint(x: 21.4, y: 20))
        body.closeSubpath()
        ctx.fill(body, with: .color(shell))
        light(ctx, body)
        var lines = Path()
        lines.move(to: CGPoint(x: 5.2, y: 5.3)); lines.addLine(to: CGPoint(x: 5.2, y: 20))
        lines.move(to: CGPoint(x: 18.8, y: 5.3)); lines.addLine(to: CGPoint(x: 18.8, y: 20))
        ctx.stroke(lines, with: .color(groove), lineWidth: 0.4)
        var panel = Path()
        panel.move(to: CGPoint(x: 6.4, y: 18.6))
        panel.addLine(to: CGPoint(x: 6.4, y: 7.2))
        panel.addQuadCurve(to: CGPoint(x: 17.6, y: 7.2), control: CGPoint(x: 12, y: 4.6))
        panel.addLine(to: CGPoint(x: 17.6, y: 18.6))
        panel.closeSubpath()
        ctx.fill(panel, with: .color(Color(white: 0.56)))
        ctx.stroke(panel, with: .color(groove), lineWidth: 0.3)
        sticker(ctx, CGRect(x: 7.4, y: 8, width: 9.2, height: 9.4), Color(red: 0.9, green: 0.75, blue: 0.2), Color(red: 0.8, green: 0.2, blue: 0.2))
    }

    /// Black: a wide grip across the top, a narrower body below with side notches and a recess.
    func genesis(_ ctx: GraphicsContext) {
        let shell = Color(white: 0.13), line = Color(white: 0.34)
        var body = Path()
        body.addRoundedRect(in: CGRect(x: 1.5, y: 3.2, width: 21, height: 8.8), cornerSize: CGSize(width: 1.4, height: 1.4))
        body.addRoundedRect(in: CGRect(x: 3.2, y: 10, width: 17.6, height: 11), cornerSize: CGSize(width: 1.2, height: 1.2))
        ctx.fill(body, with: .color(shell))
        light(ctx, body)
        ctx.stroke(Path(roundedRect: CGRect(x: 1.5, y: 3.2, width: 21, height: 8.8), cornerRadius: 1.4), with: .color(line), lineWidth: 0.3)
        ctx.stroke(Path(roundedRect: CGRect(x: 3.2, y: 10, width: 17.6, height: 11), cornerRadius: 1.2), with: .color(line), lineWidth: 0.3)
        ctx.fill(Path(CGRect(x: 3.1, y: 18, width: 0.9, height: 0.9)), with: .color(line))
        ctx.fill(Path(CGRect(x: 20, y: 18, width: 0.9, height: 0.9)), with: .color(line))
        var recess = Path()
        recess.move(to: CGPoint(x: 7, y: 17))
        recess.addLine(to: CGPoint(x: 17, y: 17))
        recess.addLine(to: CGPoint(x: 15.8, y: 20.2))
        recess.addLine(to: CGPoint(x: 8.2, y: 20.2))
        recess.closeSubpath()
        ctx.stroke(recess, with: .color(line), lineWidth: 0.3)
        ctx.fill(Path(CGRect(x: 2.4, y: 4.2, width: 19.2, height: 2.1)), with: .color(Color(white: 0.2)))
        sticker(ctx, CGRect(x: 2.4, y: 6.3, width: 19.2, height: 9.8), Color(red: 0.15, green: 0.4, blue: 0.9), Color(red: 0.85, green: 0.25, blue: 0.2))
    }

    /// A white shell, round on top and square-shouldered below, with the disc seen through a
    /// round window: the game's art printed around a plain hub, the title on a band at the foot.
    func umd(_ ctx: GraphicsContext) {
        let centre = CGPoint(x: 12, y: 11.5), r = 9.1, hub = 4.3
        var body = Path()
        body.move(to: CGPoint(x: 1.9, y: 11.6))
        body.addArc(center: CGPoint(x: 12, y: 11.6), radius: 10.1, startAngle: .degrees(180), endAngle: .degrees(0), clockwise: false)
        body.addCurve(to: CGPoint(x: 22.6, y: 19), control1: CGPoint(x: 22.1, y: 14.5), control2: CGPoint(x: 22.6, y: 17))
        body.addCurve(to: CGPoint(x: 19.8, y: 22.2), control1: CGPoint(x: 22.6, y: 21), control2: CGPoint(x: 21.6, y: 22))
        body.addQuadCurve(to: CGPoint(x: 4.2, y: 22.2), control: CGPoint(x: 12, y: 23.2))
        body.addCurve(to: CGPoint(x: 1.4, y: 19), control1: CGPoint(x: 2.4, y: 22), control2: CGPoint(x: 1.4, y: 21))
        body.addCurve(to: CGPoint(x: 1.9, y: 11.6), control1: CGPoint(x: 1.4, y: 17), control2: CGPoint(x: 1.9, y: 14.5))
        body.closeSubpath()
        ctx.fill(body, with: .linearGradient(Gradient(colors: [Color(white: 0.97), Color(white: 0.84)]),
                                             startPoint: CGPoint(x: 0, y: 1.5), endPoint: CGPoint(x: 0, y: 23)))
        ctx.stroke(body, with: .color(Color(white: 0.6)), lineWidth: 0.15)

        let circle = { (radius: Double) in CGRect(x: centre.x - radius, y: centre.y - radius, width: radius * 2, height: radius * 2) }
        ctx.fill(Path(ellipseIn: circle(r + 0.45)), with: .color(Color(white: 0.3)))
        ctx.fill(Path(ellipseIn: circle(r + 0.2)), with: .color(Color(white: 0.72)))

        var disc = ctx
        disc.clip(to: Path(ellipseIn: circle(r)))
        printed(disc, circle(r), Color(red: 0.25, green: 0.3, blue: 0.5), Color(red: 0.75, green: 0.3, blue: 0.2))
        let band = CGRect(x: centre.x - r, y: centre.y + 4.9, width: r * 2, height: r - 4.9)
        if let title {
            disc.fill(Path(band), with: .color(Color(red: 0.62, green: 0.13, blue: 0.1).opacity(0.92)))
            let words = title.split(separator: " ")
            var first = words.count, length = 0
            for (i, w) in words.enumerated() where length < title.count / 2 { length += w.count + 1; first = i + 1 }
            let lines = [words[..<first], words[first...]].map { $0.joined(separator: " ") }.filter { !$0.isEmpty }
            for (i, line) in lines.enumerated() {
                disc.draw(Text(line).font(.system(size: 0.8, weight: .heavy, design: .rounded)).foregroundColor(.white),
                          at: CGPoint(x: centre.x, y: band.minY + (lines.count == 1 ? 1.5 : 0.95 + Double(i))))
            }
        }
        ctx.fill(Path(ellipseIn: circle(hub + 0.25)), with: .color(.black.opacity(0.2)))
        ctx.fill(Path(ellipseIn: circle(hub)), with: .color(Color(white: 0.95)))
        ctx.draw(Text("UMD").font(.system(size: 2.3, weight: .regular)).foregroundColor(Color(red: 0.25, green: 0.45, blue: 0.75)), at: centre)

        var glass = ctx
        glass.clip(to: Path(ellipseIn: circle(r)))
        glass.fill(Path(ellipseIn: circle(r).offsetBy(dx: -3.5, dy: -6)), with: .color(.white.opacity(0.09)))

        ctx.draw(Text("PSP").font(.system(size: 0.95, weight: .semibold)).foregroundColor(Color(white: 0.55)), at: CGPoint(x: 4.1, y: 20.6))
        var pad = Path()
        pad.addRoundedRect(in: CGRect(x: 18.9, y: 19.9, width: 2.3, height: 0.95), cornerSize: CGSize(width: 0.4, height: 0.4))
        pad.addEllipse(in: CGRect(x: 18.7, y: 20.3, width: 0.8, height: 1.1))
        pad.addEllipse(in: CGRect(x: 20.6, y: 20.3, width: 0.8, height: 1.1))
        ctx.fill(pad, with: .color(Color(white: 0.55)))
    }

    func plain(_ ctx: GraphicsContext) {
        ctx.fill(Path(roundedRect: CGRect(x: 4, y: 4, width: 16, height: 16), cornerRadius: 2), with: .color(Color(white: 0.4)))
    }
}
