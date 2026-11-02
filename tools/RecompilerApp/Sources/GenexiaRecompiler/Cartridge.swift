import SwiftUI

/// A small drawing of the console's cartridge: its shape, no logos.
struct Cartridge: View {
    let console: String
    var body: some View {
        Canvas { ctx, size in
            let u = min(size.width, size.height) / 24
            ctx.translateBy(x: (size.width - 24 * u) / 2, y: (size.height - 24 * u) / 2)
            ctx.scaleBy(x: u, y: u)
            switch console {
            case "SNES": snes(ctx)
            case "Nintendo 64": n64(ctx)
            case "Genesis": genesis(ctx)
            default: plain(ctx)
            }
        }
        .aspectRatio(1, contentMode: .fit)
    }

    func sticker(_ ctx: GraphicsContext, _ r: CGRect, _ top: Color, _ bottom: Color) {
        ctx.fill(Path(roundedRect: r, cornerRadius: 0.4), with: .linearGradient(Gradient(colors: [top, bottom]),
            startPoint: CGPoint(x: r.minX, y: r.minY), endPoint: CGPoint(x: r.maxX, y: r.maxY)))
    }

    /// North American cartridge: wide and flat-topped, with ribbed side wings around a raised centre.
    func snes(_ ctx: GraphicsContext) {
        let wing = Color(white: 0.42), edge = Color(white: 0.28), centre = Color(white: 0.5)
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
        let shell = Color(white: 0.56), groove = Color(white: 0.36)
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
        ctx.fill(panel, with: .color(Color(white: 0.5)))
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

    func plain(_ ctx: GraphicsContext) {
        ctx.fill(Path(roundedRect: CGRect(x: 4, y: 4, width: 16, height: 16), cornerRadius: 2), with: .color(Color(white: 0.4)))
    }
}
