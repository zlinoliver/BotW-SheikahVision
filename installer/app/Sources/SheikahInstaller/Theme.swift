import AppKit
import SwiftUI

enum Theme {
    static let warning = Color(red: 1, green: 0.62, blue: 0.24)
    static let accent = Color(red: 54 / 255, green: 197 / 255, blue: 240 / 255)
    /// the glow of the app icon's eye
    static let glow = Color(red: 50 / 255, green: 250 / 255, blue: 250 / 255)
}

/// The headset app's icon (its three layers flattened, round), from Resources/logo.png.
struct AppLogo: View {
    var size: CGFloat
    var body: some View {
        Image(nsImage: NSImage(contentsOf: Bundle.module.url(forResource: "logo", withExtension: "png")!)!)
            .resizable().interpolation(.high).frame(width: size, height: size).clipShape(Circle()).accessibilityHidden(true)
    }
}

/// Teams as a short list of rows (the system pop-up looked out of place on the glass card).
struct TeamChooser: View {
    let teams: [Team]
    @Binding var selection: String
    var body: some View {
        VStack(spacing: 4) {
            ForEach(teams) { team in
                let chosen = team.id == selection
                Button { selection = team.id } label: {
                    HStack(spacing: 10) {
                        Image(systemName: chosen ? "checkmark.circle.fill" : "circle")
                            .foregroundStyle(chosen ? Theme.glow : .white.opacity(0.45))
                        Text(team.name).font(.system(size: 13, weight: .semibold)).lineLimit(1)
                        Spacer(minLength: 8)
                        Text(team.paid ? text("付费账号 · 1 年", "Paid · 1 year") : text("免费账号 · 7 天", "Free · 7 days"))
                            .font(.system(size: 11)).foregroundStyle(.white.opacity(0.6))
                    }
                    .padding(.horizontal, 12).frame(height: 30).contentShape(Rectangle())
                    .background(.white.opacity(chosen ? 0.12 : 0.05), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
                    .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).stroke(chosen ? Theme.glow.opacity(0.7) : .white.opacity(0.08), lineWidth: 1))
                }.buttonStyle(.plain)
            }
        }
    }
}

extension View {
    @ViewBuilder func installerGlass<S: Shape>(_ shape: S) -> some View {
        if #available(macOS 26, *) {
            background(shape.fill(.black.opacity(0.40))).glassEffect(.regular, in: shape)
        } else {
            background(.ultraThinMaterial, in: shape)
                .background(shape.fill(.black.opacity(0.65)))
                .overlay(shape.stroke(.white.opacity(0.14), lineWidth: 1))
        }
    }
}

// The installer's own backdrop (a plain landscape, no text, chosen 2026-11-11; the launcher keeps CoverOriginal).
// Its left side is dark and empty, where the cards sit; the veil keeps them readable.
struct CoverBackdrop: View {
    var body: some View {
        GeometryReader { geometry in
            Image(nsImage: NSImage(contentsOf: Bundle.module.url(forResource: "backdrop", withExtension: "jpg")!)!)
                .resizable().scaledToFill()
                .frame(width: geometry.size.width, height: geometry.size.height)
                .clipped()
                .overlay {
                    LinearGradient(stops: [
                        .init(color: .black.opacity(0.20), location: 0),
                        .init(color: .clear, location: 0.25),
                        .init(color: .clear, location: 0.55),
                        .init(color: .black.opacity(0.50), location: 1)
                    ], startPoint: .top, endPoint: .bottom)
                }
                .clipped()
        }.accessibilityHidden(true)
    }
}

struct InstallerTitle: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("SheikahVision").font(.system(size: 40, weight: .bold, design: .rounded))
            Text(text("在 Apple Vision Pro 上以原生 VR 玩《塞尔达传说 旷野之息》", "Play The Legend of Zelda: Breath of the Wild in native VR on Apple Vision Pro"))
                .font(.system(size: 14, weight: .medium)).opacity(0.85)
        }.foregroundStyle(.white).shadow(color: .black.opacity(0.45), radius: 8, y: 2).accessibilityElement(children: .combine)
    }
}

struct InstallerFooter: View {
    /// the app bundle's version (build_installer.sh writes the repository VERSION into Info.plist); "dev" under swift run
    static var version: String { Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "dev" }
    var body: some View {
        HStack {
            Text(text("非官方粉丝项目 · 与任天堂无关", "Unofficial fan project · Not affiliated with Nintendo"))
            Spacer()
            Text(text("版本 ", "Version ") + Self.version)
        }.font(.system(size: 10)).foregroundStyle(.white.opacity(0.55)).lineLimit(1)
    }
}

struct InstallerMark: View {
    var size: CGFloat = 52
    var body: some View {
        RoundedRectangle(cornerRadius: size * 0.23).fill(Theme.accent.gradient)
            .overlay(Image(systemName: "viewfinder").font(.system(size: size * 0.6, weight: .medium)).foregroundStyle(.black.opacity(0.8)))
            .frame(width: size, height: size).accessibilityHidden(true)
    }
}

struct StatusMark: View {
    let state: StepState
    var body: some View {
        Group {
            switch state {
            case .done: Image(systemName: "checkmark").foregroundStyle(Theme.accent)
            case .working: ProgressView().controlSize(.mini)
            case .attention: Circle().fill(Theme.warning).frame(width: 6, height: 6)
            case .todo, .waiting: Circle().fill(.white.opacity(0.2)).frame(width: 4, height: 4)
            }
        }.frame(width: 12, height: 14)
    }
}

struct StepCard<Content: View, Footer: View>: View {
    var title: String
    var subtitle: String? = nil
    var width: CGFloat = 540
    @ViewBuilder var content: Content
    @ViewBuilder var footer: Footer
    var body: some View {
        VStack(alignment: .leading, spacing: 15) {
            Text(title).font(.system(size: 25, weight: .bold)).fixedSize(horizontal: false, vertical: true)
            if let subtitle { Text(subtitle).font(.system(size: 14)).foregroundStyle(.white.opacity(0.78)).fixedSize(horizontal: false, vertical: true) }
            content
            HStack(spacing: 16) { footer }
        }
        .foregroundStyle(.white).padding(23).frame(width: width, alignment: .leading)
        .installerGlass(RoundedRectangle(cornerRadius: 28, style: .continuous))
        .shadow(color: .black.opacity(0.25), radius: 22, y: 8)
    }
}

struct ProgressRing: View {
    let fraction: Double
    var body: some View {
        ZStack {
            Circle().stroke(.white.opacity(0.10), lineWidth: 7)
            Circle().trim(from: 0, to: max(0.001, min(1, fraction)))
                .stroke(Theme.glow, style: StrokeStyle(lineWidth: 7, lineCap: .round)).rotationEffect(.degrees(-90))
                .shadow(color: Theme.glow.opacity(0.6), radius: 6)
            AppLogo(size: 78)
        }.frame(width: 108, height: 108).padding(5)
            .accessibilityLabel(text("安装进度", "Installation progress"))
            .accessibilityValue("\(Int(fraction * 100))%")
    }
}


/// Secondary action: an outlined glass capsule beside the white primary pill, the same height.
struct InstallerSecondaryPill: ButtonStyle {
    @Environment(\.isEnabled) private var enabled
    func makeBody(configuration: Configuration) -> some View {
        configuration.label.font(.system(size: 13, weight: .semibold)).lineLimit(1).fixedSize()
            .foregroundStyle(.white.opacity(enabled ? 1 : 0.4)).padding(.horizontal, 16).frame(minHeight: 40)
            .background(.white.opacity(configuration.isPressed ? 0.18 : 0.08), in: Capsule())
            .overlay(Capsule().stroke(.white.opacity(0.22), lineWidth: 1))
            .contentShape(Capsule())
    }
}

struct InstallerPill: ButtonStyle {
    @Environment(\.isEnabled) private var enabled
    func makeBody(configuration: Configuration) -> some View {
        configuration.label.font(.system(size: 14, weight: .semibold))
            .foregroundStyle(.black).padding(.horizontal, 22).frame(minHeight: 40)
            .background(.white.opacity(enabled ? (configuration.isPressed ? 0.8 : 1) : 0.35), in: Capsule())
            .contentShape(Capsule())
    }
}
