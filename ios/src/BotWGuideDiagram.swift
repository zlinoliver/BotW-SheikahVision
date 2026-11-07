// Original vector field guide for SheikahVision. All coordinates are in a 1100 pt design space.
import SwiftUI

// the palette of the controls illustration (docs/images/controls.png) above the plates: cream paper, greens
// (names kept from the first, dark version: cyan = lines and headings, gold = titles and accents, ink = body text and
// figures, blue = the plate's paper)
private let guideCyan = Color(red: 0.21, green: 0.45, blue: 0.36)
private let guideGold = Color(red: 0.09, green: 0.27, blue: 0.21)
private let guideInk = Color(red: 0.30, green: 0.36, blue: 0.33)
private let guideBlue = Color(red: 0.99, green: 0.98, blue: 0.95)

struct BotWGuideDiagram: View {

    var body: some View {
        VStack(spacing: 18) {
            // the buttons and gestures at a glance: the same illustration as the README and the website (user
            // 2026-11-13); the plates under it keep the details it leaves out
            GuideControlsImage()
            plate("g.equip", index: "g.01", height: 560) { EquipmentPlate() }
            plate("g.combat", index: "g.02", height: 490) { CombatPlate() }
            plate("g.other", index: "g.03", height: 540) { OtherPlate() }
        }
        .environment(\.colorScheme, .light)
    }

    private func plate<C: View>(_ title: String, index: String, height: CGFloat,
                                @ViewBuilder content: @escaping () -> C) -> some View {
        BotWFitWidth(width: 1100, height: height) {
            ZStack(alignment: .topLeading) {
                RoundedRectangle(cornerRadius: 22).fill(guideBlue)
                RoundedRectangle(cornerRadius: 22).strokeBorder(guideCyan.opacity(0.35), lineWidth: 1)
                GuideHeading(title: title, index: index).position(x: 550, y: 35)
                Path { p in p.move(to: CGPoint(x: 26, y: 65)); p.addLine(to: CGPoint(x: 1074, y: 65)) }
                    .stroke(guideCyan.opacity(0.25), lineWidth: 1)
                content()
            }
        }
    }
}

/// The controls illustration in the app's language; its small print is about 13 pt at the guide's width, so a tap
/// opens it larger, to pan with a drag and zoom with a pinch.
private struct GuideControlsImage: View {
    @State private var enlarged = false
    var body: some View {
        if let image = guideControlsImage() {
            Button { enlarged = true } label: {
                Image(uiImage: image).resizable().aspectRatio(contentMode: .fit)
                    .clipShape(RoundedRectangle(cornerRadius: 22))
                    .overlay(alignment: .bottomTrailing) {
                        Label(tr("g.enlarge"), systemImage: "arrow.up.left.and.arrow.down.right")
                            .font(.callout.weight(.semibold)).padding(.horizontal, 14).padding(.vertical, 8)
                            .background(.regularMaterial, in: Capsule()).padding(16)
                    }
            }
            .buttonStyle(.plain)
            .frame(maxWidth: 1100).frame(maxWidth: .infinity)
            .accessibilityLabel(tr("g.controllers"))
            .sheet(isPresented: $enlarged) { GuideControlsZoom(image: image) }
        }
    }
}

private func guideControlsImage() -> UIImage? {
    var name = "GuideControls-en"
    return UIImage(named: name)
}

private struct GuideControlsZoom: View {
    let image: UIImage
    @Environment(\.dismiss) private var dismiss
    @State private var zoom: CGFloat = 1.6
    @State private var base: CGFloat = 1.6
    var body: some View {
        NavigationStack {
            GeometryReader { g in
                ScrollView([.horizontal, .vertical]) {
                    Image(uiImage: image).resizable().aspectRatio(contentMode: .fit)
                        .frame(width: g.size.width * zoom)
                }
                .gesture(MagnifyGesture()
                    .onChanged { zoom = min(max(base * $0.magnification, 1), 3) }
                    .onEnded { _ in base = zoom })
            }
            .background(guideBlue)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(tr("g.close"), systemImage: "xmark") { dismiss() }
                }
            }
        }
        .frame(minWidth: 1200, minHeight: 760)
    }
}

struct BotWFitWidth<Content: View>: View {
    let width: CGFloat, height: CGFloat
    @ViewBuilder let content: () -> Content
    var body: some View {
        GeometryReader { g in
            content().environment(\.guideDrawingScale, g.size.width / width)
                .frame(width: width, height: height)
                .scaleEffect(g.size.width / width, anchor: .topLeading)
                .frame(width: g.size.width, height: g.size.height, alignment: .topLeading)
        }
        .aspectRatio(width / height, contentMode: .fit)
        .frame(maxWidth: width).frame(maxWidth: .infinity)
    }
}

// Keep section headings at 24 screen points while the illustration scales.
private struct GuideScaleKey: EnvironmentKey { static let defaultValue: CGFloat = 1 }
private extension EnvironmentValues {
    var guideDrawingScale: CGFloat {
        get { self[GuideScaleKey.self] }
        set { self[GuideScaleKey.self] = newValue }
    }
}
private struct GuideHeading: View {
    let title: String
    let index: String
    @Environment(\.guideDrawingScale) private var scale
    var body: some View {
        HStack(spacing: 16) {
            Text(tr(index)).foregroundStyle(guideGold)
            Text(tr(title)).foregroundStyle(guideCyan)
        }
        .font(.system(size: 24 / max(scale, 0.1), weight: .semibold, design: .rounded))
    }
}

/// Explicit line breaks keep the guide text in the same shape at every scale.
private struct GuideCopy: View {
    let title: String
    let detail: String
    var width: CGFloat
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(tr(title)).font(.system(size: 23, weight: .semibold)).foregroundStyle(guideGold)
            Text(tr(detail)).font(.system(size: 22)).foregroundStyle(guideInk)
                .lineSpacing(2)
        }
        .fixedSize(horizontal: true, vertical: true)
        .frame(width: width, alignment: .leading)
    }
}

private struct EquipmentPlate: View {
    var body: some View {
        ZStack(alignment: .topLeading) {
            Text(tr("g.equipRule")).font(.system(size: 22)).foregroundStyle(guideCyan)
                .multilineTextAlignment(.center).position(x: 550, y: 112)
            Canvas { c, _ in
                // the back view (its left on the left) carries the slots; the side view beside it shows the right
                // hand reaching over the right shoulder, next to that slot's label
                person(c, x: 500, y: 210, pose: .back, scale: 0.82)
                person(c, x: 650, y: 210, pose: .shoulder, scale: 0.82)
                for (x, y) in [(CGFloat(469), CGFloat(296)), (531, 296), (529, 370)] {
                    dot(c, x, y, color: guideCyan.opacity(0.2), radius: 15)
                    dot(c, x, y, color: guideGold, radius: 5)
                }
                stroke(c, [(366, 254), (386, 254), (469, 296)], color: guideCyan)
                stroke(c, [(734, 254), (712, 254), (676, 286)], color: guideCyan)
                stroke(c, [(734, 451), (714, 451), (566, 451), (529, 370)], color: guideCyan)
                arrow(c, from: (680, 335), to: (667, 280), color: guideGold)
            }
            GuideCopy(title: "g.lsh", detail: "g.lsh.d", width: 340).position(x: 196, y: 270)
            GuideCopy(title: "g.rsh", detail: "g.rsh.d", width: 340).position(x: 904, y: 284)
            GuideCopy(title: "g.throw", detail: "g.throw.d", width: 340).position(x: 196, y: 456)
            GuideCopy(title: "g.rhip", detail: "g.rhip.d", width: 340).position(x: 904, y: 456)
            Text(tr("g.behind")).position(x: 500, y: 191)
            Text(tr("g.reach")).position(x: 650, y: 191)
        }.font(.system(size: 22)).foregroundStyle(guideCyan)
    }
}

private struct CombatPlate: View {
    var body: some View {
        ZStack(alignment: .topLeading) {
            Canvas { c, _ in
                person(c, x: 180, y: 88, pose: .sword, scale: 0.83)
                person(c, x: 550, y: 88, pose: .shield, scale: 0.83)
                person(c, x: 920, y: 88, pose: .twoHands, scale: 0.83)
                var arc = Path()
                arc.addArc(center: CGPoint(x: 178, y: 199), radius: 104, startAngle: .degrees(-110), endAngle: .degrees(15), clockwise: false)
                c.stroke(arc, with: .color(guideGold), style: StrokeStyle(lineWidth: 2, dash: [7, 5]))
                arrow(c, from: (568, 234), to: (619, 234), color: guideGold)
            }
            GuideCopy(title: "g.swing", detail: "g.c1", width: 316).position(x: 184, y: 366)
            GuideCopy(title: "g.guard", detail: "g.guard.d", width: 316).position(x: 550, y: 400)
            GuideCopy(title: "g.twoHands", detail: "g.c5", width: 316).position(x: 916, y: 378)
        }
    }
}

private struct OtherPlate: View {
    var body: some View {
        ZStack(alignment: .topLeading) {
            Canvas { c, _ in
                person(c, x: 180, y: 90, pose: .whistle, scale: 0.78)
                person(c, x: 550, y: 90, pose: .magnet, scale: 0.78)
                person(c, x: 850, y: 90, pose: .bow, scale: 0.7)
                arrow(c, from: (628, 160), to: (628, 110), color: guideCyan)
                arrow(c, from: (628, 170), to: (628, 220), color: guideCyan)
                arrow(c, from: (601, 252), to: (657, 231), color: guideGold)
                arrow(c, from: (591, 258), to: (535, 279), color: guideGold)
                shape(c, CGRect(x: 644, y: 155, width: 36, height: 36), radius: 5, tint: guideCyan)
                // the arrow's path, the way the archer shoots: rising, falling from its top (red) to where it lands
                let a = CGPoint(x: 775, y: 345), k = CGPoint(x: 915, y: 270), b = CGPoint(x: 1060, y: 368)
                var arc = Path(); arc.move(to: a); arc.addQuadCurve(to: b, control: k)
                c.stroke(arc, with: .color(guideInk), lineWidth: 2)
                let t = (a.y - k.y) / (a.y - 2 * k.y + b.y), u = 1 - t
                dot(c, u * u * a.x + 2 * u * t * k.x + t * t * b.x, u * u * a.y + 2 * u * t * k.y + t * t * b.y,
                    color: .red, radius: 6)
                dot(c, b.x, b.y, color: Color(red: 0.3, green: 0.6, blue: 1.0), radius: 6)
            }
            GuideCopy(title: "g.whistle", detail: "g.o1", width: 316).position(x: 184, y: 398)
            GuideCopy(title: "g.magnesis", detail: "g.o2", width: 316).position(x: 550, y: 398)
            GuideCopy(title: "g.bow", detail: "g.o3", width: 316).position(x: 916, y: 424)
            VStack(alignment: .leading, spacing: 4) {
                Text(tr("g.arc")).foregroundStyle(guideInk)
                HStack(spacing: 18) {
                    Text(tr("g.fall")).foregroundStyle(Color.red.opacity(0.9))
                    Text(tr("g.land")).foregroundStyle(Color(red: 0.4, green: 0.7, blue: 1.0))
                }
            }
            .font(.system(size: 22)).fixedSize().frame(width: 316, alignment: .leading).position(x: 916, y: 502)
        }
    }
}

// MARK: - Original chibi VR explorer, with deliberately simple non-character clothing.
private enum GuidePose { case shoulder, back, sword, shield, twoHands, whistle, magnet, bow }

private func person(_ context: GraphicsContext, x: CGFloat, y: CGFloat, pose: GuidePose, scale: CGFloat) {
    var c = context
    c.translateBy(x: x, y: y); c.scaleBy(x: scale, y: scale)
    // the archer holds the bow in its left hand: on the right as it faces us, shooting to the right
    if pose == .bow { c.scaleBy(x: -1, y: 1) }
    // Oversized head, short boots, plain jumpsuit and a wraparound visor.
    stroke(c, [(-22, 208), (-30, 263), (-48, 263)], color: guideInk, width: 14)
    stroke(c, [(22, 208), (32, 263), (50, 263)], color: guideInk, width: 14)
    shape(c, CGRect(x: pose == .shoulder ? -32 : -43, y: 100, width: pose == .shoulder ? 64 : 86, height: 119), radius: 26)
    stroke(c, [(-38, 189), (38, 189)], color: guideCyan, width: 2)
    let head = Path(ellipseIn: CGRect(x: pose == .shoulder ? -28 : -44, y: 7, width: pose == .shoulder ? 68 : 88, height: 88))
    c.fill(head, with: .color(guideBlue)); c.stroke(head, with: .color(guideInk), lineWidth: 2)
    if pose == .back {
        stroke(c, [(-43, 46), (43, 46)], color: guideCyan, width: 7)
        shape(c, CGRect(x: -22, y: 33, width: 44, height: 27), radius: 10, tint: guideCyan)
    } else {
        shape(c, CGRect(x: pose == .shoulder ? 0 : -48, y: 32, width: pose == .shoulder ? 52 : 96, height: 38), radius: 14, tint: guideCyan)
        stroke(c, [(pose == .shoulder ? 12 : -27, 43), (pose == .shoulder ? 30 : -9, 43)], color: guideCyan, width: 2)
    }
    var arms: [[(CGFloat, CGFloat)]]
    switch pose {
    case .shoulder: arms = [[(-38, 114), (-62, 158), (-37, 192)], [(38, 114), (70, 77), (29, 93)]]
    case .back: arms = [[(-38, 114), (-68, 154), (-35, 191)], [(38, 114), (70, 156), (37, 191)]]
    case .sword: arms = [[(-38, 114), (-69, 146), (-81, 177)], [(38, 114), (73, 94), (104, 62)]]
    case .shield: arms = [[(-38, 114), (-60, 141), (-16, 148)], [(38, 114), (64, 148), (88, 148)]]
    case .twoHands: arms = [[(-38, 114), (-57, 157), (7, 156)], [(38, 114), (59, 141), (37, 128)]]
    case .whistle: arms = [[(-38, 114), (-71, 118), (-22, 80)], [(38, 114), (71, 118), (22, 80)]]
    case .magnet: arms = [[(-38, 114), (-58, 156), (-61, 189)], [(38, 114), (76, 137), (102, 119)]]
    case .bow: arms = [[(-38, 114), (-83, 116), (-128, 114)], [(38, 114), (76, 126), (26, 89)]]
    }
    for arm in arms {
        stroke(c, arm, color: guideInk, width: 13)
        if let hand = arm.last { dot(c, hand.0, hand.1, color: guideGold, radius: 9) }
    }
    switch pose {
    case .sword:
        stroke(c, [(91, 77), (151, 0)], color: guideCyan, width: 5)
        stroke(c, [(88, 54), (118, 77)], color: guideGold, width: 3)
    case .shield:
        var p = Path(); p.move(to: CGPoint(x: -34, y: 116)); p.addLines([CGPoint(x: 18, y: 116), CGPoint(x: 18, y: 164), CGPoint(x: -8, y: 187), CGPoint(x: -34, y: 164)]); p.closeSubpath()
        c.fill(p, with: .color(guideBlue)); c.stroke(p, with: .color(guideCyan), lineWidth: 2)
    case .twoHands: stroke(c, [(-13, 210), (74, 62)], color: guideCyan, width: 6)
    case .bow:
        var p = Path(); p.move(to: CGPoint(x: -129, y: 46)); p.addQuadCurve(to: CGPoint(x: -129, y: 189), control: CGPoint(x: -187, y: 114))
        c.stroke(p, with: .color(guideGold), lineWidth: 3)
        stroke(c, [(-129, 46), (26, 89), (-129, 189)], color: guideInk, width: 1.5)
        arrow(c, from: (-110, 95), to: (17, 95), color: guideCyan)
    default: break
    }
}

private func stroke(_ c: GraphicsContext, _ points: [(CGFloat, CGFloat)], color: Color, width: CGFloat = 1.5) {
    guard let first = points.first else { return }
    var p = Path(); p.move(to: CGPoint(x: first.0, y: first.1))
    for point in points.dropFirst() { p.addLine(to: CGPoint(x: point.0, y: point.1)) }
    c.stroke(p, with: .color(color), style: StrokeStyle(lineWidth: width, lineCap: .round, lineJoin: .round))
}
private func shape(_ c: GraphicsContext, _ rect: CGRect, radius: CGFloat, tint: Color = guideInk) {
    let p = Path(roundedRect: rect, cornerRadius: radius)
    c.fill(p, with: .color(guideBlue)); c.fill(p, with: .color(tint.opacity(0.09)))
    c.stroke(p, with: .color(tint), lineWidth: 1.7)
}
private func dot(_ c: GraphicsContext, _ x: CGFloat, _ y: CGFloat, color: Color, radius: CGFloat) {
    c.fill(Path(ellipseIn: CGRect(x: x - radius, y: y - radius, width: radius * 2, height: radius * 2)), with: .color(color))
}
private func arrow(_ c: GraphicsContext, from: (CGFloat, CGFloat), to: (CGFloat, CGFloat), color: Color) {
    stroke(c, [from, to], color: color, width: 2)
    let a = atan2(to.1 - from.1, to.0 - from.0)
    stroke(c, [(to.0 - 10 * cos(a - 0.5), to.1 - 10 * sin(a - 0.5)), to,
               (to.0 - 10 * cos(a + 0.5), to.1 - 10 * sin(a + 0.5))], color: color, width: 2)
}
