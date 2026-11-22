// The launcher window (visionOS): what the app opens to before the game starts, after the Klepton launchers
// (the standalone Vision Pro app porting notes, sections 2, 8 and 14; Klepton visionos/Sources/KleptonGameLauncher*.swift),
// written for this app: SDL owns the UIApplication, so ios/src/main_ios.mm calls botw_launcher_run() on the main
// thread before the game starts and it spins the run loop until the player taps Start. Pages: home (start), settings
// (launch options, kept in UserDefaults and handed to the runtime as environment variables), saves (export / import
// with a backup first), game files (check, import) and a guide. Once the game runs, its own window shows a cover
// (botw_launcher_cover) until BotWImmersive.swift closes it, the game showing in the headset.
// BOTW_LAUNCHER=0 skips it (launches from the Mac for tests).
import SwiftUI
import UIKit
import UniformTypeIdentifiers

// MARK: - places

enum BotWPaths {
    static var documents: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }
    static var userRoot: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("SheikahVision")
    }
    /// A USA copy of the game in Documents/game-us is played by default, with its own saves (save-us). Launching with
    /// BOTW_GAME_FOLDER=game picks the imported copy instead.
    static var usCopy: Bool {
        let chosen = ProcessInfo.processInfo.environment["BOTW_GAME_FOLDER"]
        return (chosen == nil || chosen == "game-us") && FileManager.default.fileExists(
            atPath: documents.appendingPathComponent("game-us/code/U-King.rpx").path)
    }
    static var saves: URL { userRoot.appendingPathComponent(usCopy ? "save-us" : "save") }
    /// the experience save's own folder beside the player's (BotWLaunchSettings.experienceSave)
    static var experienceFolderName: String { (usCopy ? "save-us" : "save") + "-experience" }
    static var experienceSaves: URL { userRoot.appendingPathComponent(experienceFolderName) }
    static var saveBackups: URL { userRoot.appendingPathComponent("save-backups") }
    static var game: URL { documents.appendingPathComponent("game") }
}

// MARK: - settings

/// Launch options. Each one becomes an environment variable for the runtime (setenv before the game starts; a
/// variable the launch already carries, from Xcode or devicectl, is left as it is).
final class BotWLaunchSettings: ObservableObject {
    /// The chip, from the model identifier (the porting manual's LauncherDeviceInfo.verifiedChips): M5 (RealityDevice17,1)
    /// has the GPU for a sharper picture; M2 (RealityDevice14,1) and anything unknown get lighter presets
    static let isM5: Bool = {
        var size = 0
        sysctlbyname("hw.machine", nil, &size, nil, 0)
        var model = [CChar](repeating: 0, count: max(size, 1))
        sysctlbyname("hw.machine", &model, &size, nil, 0)
        let id = String(cString: model)
        NSLog("[launcher] device \(id)")
        return id == "RealityDevice17,1"
    }()
    /// the M2 (RealityDevice14,1) by name: its own pacing, HUD follow and lighter Smooth (2026-11-20, issue #12) need a
    /// positive match, so an unknown model or a failed lookup keeps the behaviour of 0.1.x (Fable's review 2026-11-21)
    static let isM2: Bool = {
        var size = 0
        sysctlbyname("hw.machine", nil, &size, nil, 0)
        var model = [CChar](repeating: 0, count: max(size, 1))
        sysctlbyname("hw.machine", &model, &size, nil, 0)
        return String(cString: model) == "RealityDevice14,1"
    }()

    enum Quality: String, CaseIterable, Identifiable {
        case smooth, standard, sharp
        var id: String { rawValue }
        var title: String { tr("quality." + rawValue) }
        /// render scale (each eye 1280x720 times it, then made 1.25:1 for the eye's field), by chip: M5 1.25 / 1.5 /
        /// 1.75, M2 1.0 / 1.25 / 1.5. On the M5 2.0 took the GPU 26-30 ms a game frame, 1.5 19-27 ms and 1.25 19-25 ms
        /// (device 2026-11-02 / 03): the GPU time hardly follows the pixels (Mac, the same field: 22.5 ms at 1.0, 25 at
        /// 2.0; about 5200 draws a frame for both eyes), so below 1.5 the picture only got blurrier
        var scaleValue: Double {
            switch self {
            case .smooth: return BotWLaunchSettings.isM5 ? 1.25 : 1.0
            case .standard: return BotWLaunchSettings.isM5 ? 1.5 : 1.25
            case .sharp: return BotWLaunchSettings.isM5 ? 1.75 : 1.5
            }
        }
        var scale: String { String(scaleValue) }
        /// which headset each preset is for, the same text on every device (user 2026-11-11: "M5 device" under Smooth
        /// misled an M2 owner about which one to pick)
        var detail: String { tr("quality.note." + rawValue) }
    }
    /// how far above Link's own eyes the view sits (BOTW_VR_HEIGHT), centimetres
    enum Height: Int, CaseIterable, Identifiable {
        case none = 0, ten = 10, twenty = 20, thirty = 30
        var id: Int { rawValue }
        var title: String { rawValue == 0 ? tr("height.link") : "+\(rawValue) cm" }
    }
    /// the compositor's render quality (sizes the eye textures the headset gets): the system's default (BOTW_VR_QUALITY
    /// 1.0, a factor of it: 0.52 on the device, 2026-11-04) or an absolute 0.7 / 0.85 / 1.0 (BOTW_VR_RENDER_QUALITY)
    enum CompositorResolution: String, CaseIterable, Identifiable {
        case system, high, higher, max
        var id: String { rawValue }
        var title: String { tr("compositor." + rawValue) }
        var renderQuality: String? {
            switch self {
            case .system: return nil
            case .high: return "0.7"
            case .higher: return "0.85"
            case .max: return "1.0"
            }
        }
    }
    enum Turn: String, CaseIterable, Identifiable {
        case smooth, snap30 = "30", snap45 = "45", snap60 = "60"
        var id: String { rawValue }
        var title: String { self == .smooth ? tr("turn.smooth") : tr("turn.snap", rawValue) }
    }

    private let defaults = UserDefaults.standard
    /// the developer settings (user 2026-11-05): the picture page, the game language and the test toggles stay in the
    /// code but are shown only after five taps on the home page's version line; without them the hidden settings take
    /// their defaults at launch, whatever was saved while testing
    static let devKey = "devSettings"
    static var dev: Bool { UserDefaults.standard.bool(forKey: devKey) }
    @Published var quality: Quality { didSet { defaults.set(quality.rawValue, forKey: "quality") } }
    @Published var turn: Turn { didSet { defaults.set(turn.rawValue, forKey: "turn") } }
    @Published var firstPerson: Bool { didSet { defaults.set(firstPerson, forKey: "firstPerson") } }
    /// story cutscenes (BOTW_VR_CUTSCENES): as listed (first person where BetterVR plays them so) or all on the screen
    /// in front (user 2026-11-17: in some the camera flies round and the eyes went into walls)
    @Published var cutscenesOnScreen: Bool { didSet { defaults.set(cutscenesOnScreen, forKey: "cutscenesOnScreen") } }
    /// the experience save (user 2026-11-17): the game reads and writes a folder of its own, seeded from the app's copy
    /// of the author's completed save; the player's saves are not touched, and turned off the game is back on them
    @Published var experienceSave: Bool { didSet { defaults.set(experienceSave, forKey: "experienceSave") } }
    @Published var roomscale: Bool { didSet { defaults.set(roomscale, forKey: "roomscale") } }
    @Published var sharpen: Bool { didSet { defaults.set(sharpen, forKey: "sharpen") } }
    @Published var classicButtons: Bool { didSet { defaults.set(classicButtons, forKey: "classicButtons") } }
    @Published var height: Height { didSet { defaults.set(height.rawValue, forKey: "height") } }
    /// the game's pace (BOTW_VR_PACE): every 2nd headset refresh (45 fps) or every 3rd (a steady 30, each picture shown
    /// three times, the head's turns still at 90 by the compositor's own correction). Two eyes take the GPU 20-25 ms a
    /// frame in the fields (device 2026-11-03), against 22.2 at 45: there the pictures went 2, 3, 2 refreshes, an
    /// uneven step a steady 30 does not have
    @Published var steady30: Bool { didSet { defaults.set(steady30, forKey: "steady30") } }
    /// fast GPU uploads (BOTW_BUFFER_CACHE=redirect): buffer uploads written straight into memory the GPU is done with
    /// instead of a GPU copy each, about a third less GPU time a frame on the Mac (2026-11-03); off = Cemu's own choice
    @Published var fastUploads: Bool { didSet { defaults.set(fastUploads, forKey: "fastUploads") } }
    @Published var compositorResolution: CompositorResolution {
        didSet { defaults.set(compositorResolution.rawValue, forKey: "compositorResolution") }
    }
    /// advanced picture, off by default: the game's FXAA pass replaced by a copy and its depth-of-field blur passes
    /// bypassed (BOTW_NO_FXAA 1 / BOTW_NO_DOF 1, pixel shader edits in graphic_pack_shim.cpp); with the sharpening
    /// above, each can make distant detail shimmer more
    @Published var noFxaa: Bool { didSet { defaults.set(noFxaa, forKey: "noFxaa") } }
    @Published var noDof: Bool { didSet { defaults.set(noDof, forKey: "noDof") } }

    @Published var gameLanguage: String { didSet { defaults.set(gameLanguage, forKey: "gameLanguage") } }
    @Published var availableLanguages: [String] = []
    @Published var gameRegion = ""

    func configure(_ receipt: BotWGameReceipt?) {
        availableLanguages = receipt?.languages ?? []
        gameRegion = receipt?.region ?? ""
        if !availableLanguages.contains(gameLanguage) {
            gameLanguage = BotWGameImport.defaultLanguage(region: gameRegion, available: availableLanguages)
        }
    }

    init() {
        gameLanguage = defaults.string(forKey: "gameLanguage") ?? ""
        quality = Quality(rawValue: defaults.string(forKey: "quality") ?? "") ?? Self.defaultQuality
        turn = Turn(rawValue: defaults.string(forKey: "turn") ?? "") ?? .snap30
        firstPerson = defaults.object(forKey: "firstPerson") as? Bool ?? true
        cutscenesOnScreen = defaults.object(forKey: "cutscenesOnScreen") as? Bool ?? false
        experienceSave = defaults.object(forKey: "experienceSave") as? Bool ?? false
        roomscale = defaults.object(forKey: "roomscale") as? Bool ?? false
        sharpen = defaults.object(forKey: "sharpen") as? Bool ?? false
        classicButtons = defaults.object(forKey: "classicButtons") as? Bool ?? true
        height = Height(rawValue: defaults.object(forKey: "height") as? Int ?? 20) ?? .twenty
        steady30 = defaults.object(forKey: "steady30") as? Bool ?? false
        fastUploads = defaults.object(forKey: "fastUploads") as? Bool ?? true
        // the old "compositor high quality" toggle (a Bool, the system's default quality when on) maps to System default
        // either way; the key is dropped. A quality saved under a removed preset (sharper, sharpest) reads as Standard
        // above; the old experiment keys (metalFX, lessOverscan, anisoWide, eyeShadows, mergeClears) are no longer read
        compositorResolution = CompositorResolution(rawValue: defaults.string(forKey: "compositorResolution") ?? "") ?? .higher
        defaults.removeObject(forKey: "compositorQuality")
        noFxaa = defaults.object(forKey: "noFxaa") as? Bool ?? false
        // depth of field off by default (2026-11-05: the game's blur behind a locked-on enemy blurred the headset's view
        // in fights; one eye's blur does not match a stereo view)
        noDof = defaults.object(forKey: "noDof") as? Bool ?? true
        // the measuring-time keys (narrow field 0.93, edge rings) are no longer read
        defaults.removeObject(forKey: "narrowFov")
        defaults.removeObject(forKey: "uploadDedup")
        defaults.removeObject(forKey: "edgeRings")
        // once: the classic layout (Cross jump, Triangle confirm) and fast uploads became the defaults on 2026-11-03;
        // a choice saved before that is replaced a single time, a later choice in the settings is kept
        if !defaults.bool(forKey: "defaults20261103") {
            defaults.set(true, forKey: "defaults20261103")
            classicButtons = true
            fastUploads = true
        }
    }

    /// the experience save's folder, made from the app's copy the first time (later play in it is kept); false if the
    /// app carries none or the copy failed, and the game then uses the player's own saves
    static func seedExperienceSave() -> Bool {
        let fm = FileManager.default
        let account = BotWPaths.experienceSaves.appendingPathComponent("2147483649")
        if fm.fileExists(atPath: account.appendingPathComponent("option.sav").path) { return true }
        guard let bundled = Bundle.main.resourceURL?.appendingPathComponent("experience-save"),
              fm.fileExists(atPath: bundled.appendingPathComponent("option.sav").path) else {
            NSLog("[launcher] no experience save in the app")
            return false
        }
        do {
            try fm.createDirectory(at: account.deletingLastPathComponent(), withIntermediateDirectories: true)
            if fm.fileExists(atPath: account.path) { try fm.removeItem(at: account) }
            try fm.copyItem(at: bundled, to: account)
            NSLog("[launcher] experience save set up in \(BotWPaths.experienceFolderName)")
            return true
        } catch {
            NSLog("[launcher] experience save not set up: \(error)")
            return false
        }
    }

    /// the preset a first start gets: Standard on the M5, Smooth on the M2 (2026-11-20, issue #12: an M2 at Standard ran
    /// 27-30 fps with 7-10 frames of latency, at Smooth 32-35 fps and about 5)
    static var defaultQuality: Quality { isM2 ? .smooth : .standard }

    func reset() {
        quality = Self.defaultQuality
        turn = .snap30
        firstPerson = true
        cutscenesOnScreen = false
        experienceSave = false
        roomscale = false
        sharpen = false
        classicButtons = true
        height = .twenty
        steady30 = false
        fastUploads = true
        compositorResolution = .higher
        noFxaa = false
        noDof = true
        gameLanguage = BotWGameImport.defaultLanguage(region: gameRegion, available: availableLanguages)
    }

    func apply() {
        // the hidden settings at their defaults unless the developer settings are on (reset() holds the defaults);
        // the quality offered is Standard or Smooth, a saved Sharp reads as Standard
        if !Self.dev {
            let lang = gameLanguage, q = quality == .sharp ? Quality.standard : quality, t = turn, h = height, fp = firstPerson
            let cs = cutscenesOnScreen, ex = experienceSave
            reset()
            gameLanguage = lang.isEmpty ? gameLanguage : lang
            quality = q; turn = t; height = h; firstPerson = fp; cutscenesOnScreen = cs; experienceSave = ex
        }
        if BotWPaths.usCopy {
            setenv("BOTW_GAME_FOLDER", "game-us", 0)
            setenv("BOTW_SAVE_FOLDER", "save-us", 0)
            setenv("BOTW_REGION", "US", 0)
            setenv("BOTW_LANG", "en", 0)
        }
        if !gameRegion.isEmpty, !gameLanguage.isEmpty {
            setenv("BOTW_REGION", gameRegion, 0)
            setenv("BOTW_LANG", String(gameLanguage.suffix(2)), 0)
        }
        if experienceSave, Self.seedExperienceSave() {
            setenv("BOTW_SAVE_FOLDER", BotWPaths.experienceFolderName, 1)
        }
        setenv("BOTW_SCALE", quality.scale, 0)
        setenv("BOTW_VR_TURN", turn == .smooth ? "smooth" : turn.rawValue, 0)
        setenv("BOTW_VR_FIRST_PERSON", firstPerson ? "1" : "0", 0)
        setenv("BOTW_VR_CUTSCENES", cutscenesOnScreen ? "screen" : "listed", 0)
        setenv("BOTW_VR_ROOMSCALE", roomscale ? "1" : "0", 0)
        setenv("BOTW_VR_SHARPEN", sharpen ? "0.5" : "0", 0)
        setenv("BOTW_VR_BUTTONS", classicButtons ? "classic" : "vr", 0)
        setenv("BOTW_VR_HEIGHT", String(Double(height.rawValue) / 100), 0)
        // the M2 (2026-11-20, issue #12): auto, the pace follows what the game can hold evenly (BotWImmersive.swift
        // tallyPace), and the HUD panel follows the head sooner and more gently. The M5 (the supported headset) keeps
        // every 2nd refresh and the HUD's 30 degrees, exactly as in 0.1.x
        setenv("BOTW_VR_PACE", steady30 ? "3" : Self.isM2 ? "auto" : "2", 0)
        if Self.isM2 {
            setenv("BOTW_VR_HUD_FOLLOW_DEG", "12", 0)
            setenv("BOTW_VR_HUD_FOLLOW_K", "0.1", 0)
        }
        if fastUploads {
            setenv("BOTW_BUFFER_CACHE", "redirect", 0)
            // a range bound again unchanged reuses its copy, and the upload ring is 256 MB instead of 64 (device
            // 2026-11-10, walking: 44-45 fps for two minutes instead of 41-43, no refused allocations, no artefacts)
            setenv("BOTW_REDIRECT_DEDUP", "1", 0)
            setenv("BOTW_REDIRECT_RING_MB", "256", 0)
        }
        setenv("BOTW_VR_QUALITY", "1.0", 0)
        // Smooth (meant for the M2 headset, user 2026-11-11; no M2 here, community testers measure it): the frame-rate
        // preset also keeps the targets smaller than the picture (bloom, fog, half-size lighting) at 0.75x and draws the
        // compositor's view at 0.7 instead of 0.85 (a pass that runs 90 times a second); the crop below is the same for
        // every preset (it costs no GPU). Standard keeps 1.0x and the compositor setting
        if quality == .smooth {
            // on the M2 (2026-11-20, issue #12: the GPU 100% busy at 28 fps, the compositor's view pass 2.2 ms 90 times a
            // second, about a fifth of the GPU): the view at 0.6 (the tester's note: 1.4 ms a pass there) and the
            // smaller targets at 0.5x, so the game can hold the steady 30 the auto pace switches to
            setenv("BOTW_SMALL_BUFFER_SCALE", Self.isM2 ? "0.5" : "0.75", 0)
            setenv("BOTW_VR_RENDER_QUALITY", Self.isM2 ? "0.6" : "0.7", 0)
        }
        if let q = compositorResolution.renderQuality { setenv("BOTW_VR_RENDER_QUALITY", q, 0) }
        // fixed from the headset (2026-11-05: Standard 1.5x + compositor 0.85 + 8% edge margin + merged clears: 45 fps,
        // game GPU 16-17 ms of 22, 33 ms latency): an 8% margin round each eye's view instead of 12% (BOTW_VR_OVERSCAN,
        // read once in BotWImmersive.swift) and clears merged into the next render pass (MetalRenderer.cpp). Per-eye
        // shadows, wide anisotropic filtering and MetalFX stay off; MetalFX only through BOTW_VR_METALFX from the launch
        setenv("BOTW_VR_OVERSCAN", "1.08", 0)
        setenv("BOTW_MERGE_CLEARS", "1", 0)
        // command buffers committed every 8x Cemu's draw threshold instead of 2x: fewer render passes cut short and
        // reloaded (device 2026-11-11, walking: GPU 17.2-18.0 ms a frame over 3 minutes instead of 19-20 once warm,
        // 43-45 fps, no command buffer errors)
        setenv("BOTW_COMMIT_MULT", "8", 0)
        // the targets smaller than the picture (bloom, fog, half-size lighting) stay at 1.0x whatever the quality preset
        // (device 2026-11-09, 1.5x: the user found frame rate and picture clearly better; larger than the scale turns it off)
        // 0.75x since 2026-11-11 (Standard on the headset: bloom and fog looked the same to the user)
        setenv("BOTW_SMALL_BUFFER_SCALE", "0.75", 0)
        if noFxaa { setenv("BOTW_NO_FXAA", "1", 0) }
        if noDof { setenv("BOTW_NO_DOF", "1", 0) }
        // each eye's field cropped to what the lenses show (outer, inner, top, bottom of the tangents), at the same
        // pixel count: measured with BOTW_VR_EDGE_RINGS and tuned by the user on the headset (2026-11-10: the lenses
        // showed 50-70 %; 0.6,0.7,0.7,0.8 was much sharper but the dark edge showed left, right and below, this one
        // is comfortable), with a 6 % soft edge. Distant grass, trees and hills clearly sharper at the same GPU time
        setenv("BOTW_VR_FOV", "0.7,0.8,0.7,0.9", 0)
        setenv("BOTW_VR_EDGE_FADE", "0.06", 0)
        NSLog("[launcher] start: quality \(quality.rawValue), turn \(turn.rawValue), first person \(firstPerson), "
              + "roomscale \(roomscale), classic buttons \(classicButtons), height \(height.rawValue) cm, steady 30 \(steady30), "
              + "fast uploads \(fastUploads), compositor resolution \(compositorResolution.rawValue), no FXAA \(noFxaa), "
              + "no DOF \(noDof), sharpen \(sharpen)")
    }
}

// MARK: - game files and saves

struct BotWGameCheck {
    var rpx = false, content = false, meta = false
    var bytes: Int64 = 0
    var ready: Bool { rpx && content && meta }
    var missing: [String] {
        var m: [String] = []
        if !rpx { m.append("code/U-King.rpx") }
        if !content { m.append("content/") }
        if !meta { m.append("meta/") }
        return m
    }

    /// The game folder the runtime will use (main_ios.mm find_game: Documents/game, Documents itself, or any folder
    /// in Documents holding code/U-King.rpx)
    static func folder() -> URL? {
        let fm = FileManager.default
        func has(_ u: URL) -> Bool { fm.fileExists(atPath: u.appendingPathComponent("code/U-King.rpx").path) }
        if has(BotWPaths.game) { return BotWPaths.game }
        if has(BotWPaths.documents) { return BotWPaths.documents }
        for u in (try? fm.contentsOfDirectory(at: BotWPaths.documents, includingPropertiesForKeys: nil)) ?? [] where has(u) {
            return u
        }
        return nil
    }

    static func run() -> BotWGameCheck {
        var c = BotWGameCheck()
        do {
            let engine = BotWGameImport(root: BotWPaths.documents)
            try engine.recover()
            if let receipt = try engine.receipt() {
                c.rpx = true; c.content = true; c.meta = true
                c.bytes = Int64(receipt.totalBytes)
            }
        } catch { NSLog("[launcher] game check: %@", error.localizedDescription) }
        return c
    }
}

enum BotWSaves {
    /// The account folders the game writes (save/<0x80000001 in decimal>/ holding 0..7 and option.sav)
    static func accounts() -> [URL] {
        let fm = FileManager.default
        return ((try? fm.contentsOfDirectory(at: BotWPaths.saves, includingPropertiesForKeys: nil)) ?? [])
            .filter { fm.fileExists(atPath: $0.appendingPathComponent("option.sav").path) }
    }

    static func latest() -> Date? {
        let fm = FileManager.default
        var newest: Date?
        for a in accounts() {
            for u in (try? fm.contentsOfDirectory(at: a, includingPropertiesForKeys: [.contentModificationDateKey])) ?? [] {
                let d = (try? u.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate
                if let d, newest == nil || d > newest! { newest = d }
            }
        }
        return newest
    }

    /// A save picked by the player: an account folder (with option.sav) or a folder holding one or more of them
    static func importFolder(_ picked: URL) throws -> BotWText {
        let fm = FileManager.default
        var sources: [URL] = []
        if fm.fileExists(atPath: picked.appendingPathComponent("option.sav").path) {
            sources = [picked]
        } else {
            sources = ((try? fm.contentsOfDirectory(at: picked, includingPropertiesForKeys: nil)) ?? [])
                .filter { fm.fileExists(atPath: $0.appendingPathComponent("option.sav").path) }
        }
        guard !sources.isEmpty else {
            throw NSError(domain: "botw", code: 1, userInfo: [NSLocalizedDescriptionKey: tr("saves.noneInFolder")])
        }
        // the current saves first go to save-backups/<date>
        if fm.fileExists(atPath: BotWPaths.saves.path) {
            let f = DateFormatter()
            f.dateFormat = "yyyy-MM-dd_HH-mm-ss"
            let backup = BotWPaths.saveBackups.appendingPathComponent(f.string(from: Date()))
            try fm.createDirectory(at: BotWPaths.saveBackups, withIntermediateDirectories: true)
            try fm.copyItem(at: BotWPaths.saves, to: backup)
        }
        try fm.createDirectory(at: BotWPaths.saves, withIntermediateDirectories: true)
        for s in sources {
            // a single account folder picked by itself goes to the game's own account folder name
            let name = sources.count == 1 && Int(s.lastPathComponent) == nil ? "2147483649" : s.lastPathComponent
            let dest = BotWPaths.saves.appendingPathComponent(name)
            if fm.fileExists(atPath: dest.path) { try fm.removeItem(at: dest) }
            try fm.copyItem(at: s, to: dest)
        }
        return BotWText("saves.imported", sources.count)
    }
}

/// The save folder as a document for the system's export sheet
struct BotWSaveFolder: FileDocument {
    static var readableContentTypes: [UTType] { [.folder] }
    let wrapper: FileWrapper
    init() throws { wrapper = try FileWrapper(url: BotWPaths.saves, options: .immediate) }
    init(configuration: ReadConfiguration) throws { wrapper = configuration.file }
    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper { wrapper }
}

// MARK: - the launcher

final class BotWLauncherModel: ObservableObject {
    @Published var started = false
    @Published var game = BotWGameCheck()
    @Published var checking = false
    @Published var message: BotWText?  // the alert, worded when shown (BotWText)
    @Published var busy: String?
    @Published var cancellable = false
    @Published var sources: [BotWImportSource] = []
    @Published var notices: [String] = []
    @Published var receipt: BotWGameReceipt?
    @Published var pending = false
    @Published var unfinished = false
    @Published var importProgress: BotWGameImport.Progress?
    @Published var spaceText: String?
    @Published var spaceReady = false
    @Published var hasManifest = false
    @Published var hasGameFiles = false
    private var selected: URL?
    private var scoped = false
    private var cancelToken: BotWCancelToken?
    private var backgroundTask: UIBackgroundTaskIdentifier = .invalid
    private var engine: BotWGameImport { BotWGameImport(root: BotWPaths.documents) }
    let settings = BotWLaunchSettings()
    /// auto-import (user 2026-11-12: the headset app should handle it, not leave the player to find it): with no game
    /// imported yet and a package in Documents/Import (such as SheikahVision-game.zip) or an unfinished
    /// import, the import starts by itself on launch and on coming back to the foreground, once each time; the Game
    /// Files page stays the manual path (and shows its progress)
    @Published var autoImporting = false
    private var autoImportTried = false
    private var foregroundObserver: NSObjectProtocol?

    init() {
        foregroundObserver = NotificationCenter.default.addObserver(forName: UIApplication.willEnterForegroundNotification,
                                                                    object: nil, queue: .main) { [weak self] _ in
            guard let self, !self.started else { return }
            self.autoImportTried = false
            self.check()
        }
        if let data = UserDefaults.standard.data(forKey: "gameImportSource") {
            var stale = false
            if let url = try? URL(resolvingBookmarkData: data, options: .withoutUI, relativeTo: nil, bookmarkDataIsStale: &stale) {
                selected = url; scoped = url.startAccessingSecurityScopedResource()
            }
        }
    }

    deinit { if scoped { selected?.stopAccessingSecurityScopedResource() } }

    private func clearSelection() {
        if scoped { selected?.stopAccessingSecurityScopedResource(); scoped = false }
        selected = nil
        UserDefaults.standard.removeObject(forKey: "gameImportSource")
    }

    /// the home page's version line: the game and app versions only (user 2026-11-12: the build number and commit there
    /// looked odd); the commit is on the About page, to tell builds of the same version apart
    var versionLine: String {
        let app = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
        return tr("version", app)
    }
    var aboutVersion: String {
        let info = Bundle.main.infoDictionary
        let app = info?["CFBundleShortVersionString"] as? String ?? "?"
        let commit = info?["SVSourceCommit"] as? String ?? "dev"  // 8 characters, "+" with local edits
        if commit == "dev" { return app }
        return "\(app) · \(commit.prefix(7))\(commit.hasSuffix("+") ? "+" : "")"
    }

    func check() {
        guard busy == nil, !checking else { return }
        checking = true
        DispatchQueue.global(qos: .userInitiated).async {
            let engine = self.engine
            var receipt: BotWGameReceipt?
            var errorText: String?
            do { try engine.recover(); receipt = try engine.receipt() }
            catch { errorText = error.localizedDescription }
            let pending = engine.hasPending, unfinished = engine.unfinished
            let hasGameFiles = FileManager.default.fileExists(atPath: engine.game.path)
            let hasManifest = receipt.map { (try? engine.manifest($0.titleID)) != nil } ?? false
            DispatchQueue.main.async {
                var check = BotWGameCheck()
                if let receipt { check.rpx = true; check.content = true; check.meta = true; check.bytes = Int64(receipt.totalBytes) }
                self.game = check
                self.receipt = receipt
                self.hasManifest = hasManifest
                self.hasGameFiles = hasGameFiles
                self.settings.configure(receipt)
                self.pending = pending; self.unfinished = unfinished
                if !unfinished { UserDefaults.standard.removeObject(forKey: "gameImportSource") }
                if !engine.notices.isEmpty { self.notices = engine.notices }
                if let errorText { self.notices.append(errorText) }
                self.checking = false
                self.autoImportIfNeeded()
            }
        }
    }

    private func autoImportIfNeeded() {
        guard !started, !autoImportTried, receipt == nil, pending || unfinished, busy == nil, !checking else { return }
        autoImportTried = true
        autoImporting = true
        NSLog("[launcher] auto-import: a game package is waiting (pending \(pending), unfinished \(unfinished))")
        inspectSources()
    }

    func start() {
        guard game.ready, busy == nil, !checking else { return }
        settings.apply()
        started = true
    }

    func cancelImport() { cancelToken?.cancel() }

    func inspectSources(_ url: URL? = nil) {
        guard busy == nil, !checking else { return }
        var ignoredPick = false
        if let url {
            let resuming = engine.unfinishedSource
            if let resuming, BotWZipReader.normalizedPath(resuming) != BotWZipReader.normalizedPath(url) {
                // An unfinished import keeps its source's access and bookmark; a different pick would lose them.
                ignoredPick = true
            } else {
                if scoped { selected?.stopAccessingSecurityScopedResource() }
                selected = url
                scoped = url.startAccessingSecurityScopedResource()
                // A preview is not a resumable transaction. Persist only after the engine creates its journal,
                // or now when this pick is the unfinished import's own source (re-granting its access).
                let bookmark = resuming == nil ? nil : try? url.bookmarkData(options: .minimalBookmark, includingResourceValuesForKeys: nil, relativeTo: nil)
                if let bookmark { UserDefaults.standard.set(bookmark, forKey: "gameImportSource") }
                else { UserDefaults.standard.removeObject(forKey: "gameImportSource") }
            }
        }
        spaceText = nil; spaceReady = false
        busy = tr("import.identifying")
        let selected = self.selected
        DispatchQueue.global(qos: .userInitiated).async {
            let engine = self.engine
            var sources: [BotWImportSource] = [], errorText: String?
            var spaceReady = false
            do {
                sources = try engine.discover(selected: selected)
                if let source = sources.first { try engine.previewSpace(source); spaceReady = true }
            } catch { errorText = error.localizedDescription }
            let spaceText = engine.lastFree > 0 ? tr("import.space", BotWGameImport.size(engine.lastNeeded), BotWGameImport.size(UInt64(engine.lastFree))) : nil
            DispatchQueue.main.async {
                if engine.selectionFailed { self.clearSelection() }
                self.sources = sources
                self.spaceText = spaceText; self.spaceReady = spaceReady
                self.notices = (ignoredPick ? [tr("import.finishFirst")] : []) + engine.notices + (errorText.map { [$0] } ?? [])
                if sources.isEmpty && self.notices.isEmpty { self.notices = [tr("import.none")] }
                self.busy = nil
                if self.autoImporting {
                    if !sources.isEmpty && spaceReady { self.beginImport() }  // otherwise the Game Files page tells why
                    else { self.autoImporting = false; NSLog("[launcher] auto-import: not started (\(self.notices))") }
                }
            }
        }
    }

    // Both old picker entry points now use the same identification and review step.
    func importGameZip(_ url: URL) { inspectSources(url) }
    func importGame(_ url: URL) { inspectSources(url) }

    private func endImportBackgroundTask() {
        let task = backgroundTask
        backgroundTask = .invalid
        if task != .invalid { UIApplication.shared.endBackgroundTask(task) }
    }

    func beginImport() {
        guard let source = sources.first, busy == nil, !checking, spaceReady else { return }
        let token = BotWCancelToken()
        cancelToken = token; cancellable = true
        busy = tr("import.extracting")
        UIApplication.shared.isIdleTimerDisabled = true
        let bookmark = selected.flatMap { try? $0.bookmarkData(options: .minimalBookmark, includingResourceValuesForKeys: nil, relativeTo: nil) }
        backgroundTask = UIApplication.shared.beginBackgroundTask(withName: "Game import") { [weak self] in
            token.cancel()
            DispatchQueue.main.async {
                guard let self, self.cancelToken === token else { return }
                self.endImportBackgroundTask()
            }
        }
        DispatchQueue.global(qos: .userInitiated).async {
            let engine = self.engine
            var result = BotWText("files.imported"), lastUpdate = Date.distantPast, succeeded = false
            do {
                try engine.run(source, token: token, journalReady: {
                    DispatchQueue.main.async {
                        guard self.cancelToken === token else { return }
                        if let bookmark { UserDefaults.standard.set(bookmark, forKey: "gameImportSource") }
                    }
                }) { progress in
                    let now = Date()
                    guard now.timeIntervalSince(lastUpdate) > 0.3 || progress.stage != "extracting" else { return }
                    lastUpdate = now
                    DispatchQueue.main.async { self.importProgress = progress; self.busy = tr("import." + progress.stage) }
                }
                succeeded = true
            } catch is CancellationError { result = BotWText("import.paused") }
            catch { result = BotWText("importFailed", error.localizedDescription) }
            DispatchQueue.main.async {
                UIApplication.shared.isIdleTimerDisabled = false
                self.endImportBackgroundTask()
                self.busy = nil; self.cancellable = false; self.cancelToken = nil; self.importProgress = nil
                self.autoImporting = false
                self.notices = engine.notices; self.sources = []
                self.message = result
                if succeeded { self.clearSelection() }
                self.check()
            }
        }
    }

    func maintenance(_ action: String) {
        guard busy == nil, !checking else { return }
        busy = tr("import.validating")
        DispatchQueue.global(qos: .userInitiated).async {
            var result: BotWText?
            let engine = self.engine
            do {
                if action == "abandon" { try engine.abandon() }
                else if action == "delete" { try engine.deleteGame() }
                else { try engine.deepValidate(); result = BotWText("files.complete") }
            } catch { result = BotWText(raw: error.localizedDescription) }
            DispatchQueue.main.async {
                if action == "abandon", result == nil { self.clearSelection() }
                self.notices = engine.notices
                self.busy = nil; self.message = result; self.sources = []; self.check()
            }
        }
    }
}

/// the launcher's pages, pushed from the home page (no side tab bar; the shared Klepton launcher's routes; user 2026-11-05)
enum BotWRoute: Hashable { case settings, picture, controls, language, saves, files, guide, about }

struct BotWLauncherView: View {
    @ObservedObject var model: BotWLauncherModel

    @State private var path: [BotWRoute] = []

    var body: some View {
        NavigationStack(path: $path) {
            BotWHomePage(model: model, path: $path)
                .toolbar(.hidden, for: .navigationBar)
                .navigationDestination(for: BotWRoute.self) { route in
                    switch route {
                    case .settings: BotWSettingsPage(settings: model.settings, page: nil).navigationTitle(tr("tab.settings"))
                    case .picture: BotWSettingsPage(settings: model.settings, page: .picture).navigationTitle(tr("set.picture"))
                    case .controls: BotWSettingsPage(settings: model.settings, page: .controls).navigationTitle(tr("set.controls"))
                    case .language: BotWSettingsPage(settings: model.settings, page: .language).navigationTitle(tr("import.gameLanguage"))
                    case .saves: BotWSavesPage(model: model, settings: model.settings).navigationTitle(tr("tab.saves"))
                    case .files: BotWGamePage(model: model).navigationTitle(tr("tab.files"))
                    case .guide: BotWGuidePage().navigationTitle(tr("tab.guide"))
                    case .about: BotWAboutPage(version: model.aboutVersion).navigationTitle(tr("tab.about"))
                    }
                }
        }
        .onAppear { model.check() }
        .alert(model.message?.text ?? "", isPresented: Binding(get: { model.message != nil }, set: { if !$0 { model.message = nil } })) {
            Button(tr("ok")) {}
        }
    }
}

// The home page, laid out as the shared Klepton launcher (KleptonGameLauncher.swift LauncherHomePage, the TotK app's;
// user 2026-11-05): the cover takes what the window leaves, cropped around a focal point; under it the info area,
// never squeezed: the title, the version and a status line on the left, the launch button (a white capsule),
// Settings on the right, stacked when the window is narrow
struct BotWHomePage: View {
    @ObservedObject var model: BotWLauncherModel
    @Binding var path: [BotWRoute]

    var body: some View {
        VStack(spacing: 0) {
            BotWHeroImage()
                .frame(maxWidth: .infinity, minHeight: 0, maxHeight: .infinity)
                .clipShape(RoundedRectangle(cornerRadius: 28, style: .continuous))
                .layoutPriority(0)
            infoArea
                .padding(.horizontal, 32)
                .padding(.top, 24)
                .padding(.bottom, 28)  // 8 + 20: the status line clear of the window's rounded corners
                .fixedSize(horizontal: false, vertical: true)
                .layoutPriority(1)
        }
    }

    private var infoArea: some View {
        ViewThatFits(in: .horizontal) {
            HStack(alignment: .center, spacing: 24) {
                titleBlock.frame(minWidth: 360, idealWidth: 460, maxWidth: .infinity, alignment: .leading)
                actions
            }
            VStack(alignment: .leading, spacing: 20) {
                titleBlock.frame(maxWidth: .infinity, alignment: .leading)
                actions
            }
        }
    }

    private var titleBlock: some View {
        VStack(alignment: .leading, spacing: 6) {
            // one line in either language (the English title shrinks to fit; user 2026-11-05: three lines in all)
            // the project's name, the game named descriptively under it (user 2026-11-11: "Full VR Mod" after the game's
            // full title read oddly)
            Text(tr("home.title")).font(.extraLargeTitle2.bold()).lineLimit(1).minimumScaleFactor(0.5)
            Text(tr("home.subtitle")).font(.title2).lineLimit(1).minimumScaleFactor(0.6)
            Text(model.versionLine).font(.title3).foregroundStyle(.secondary).lineLimit(1)
                .onTapGesture(count: 5) {
                    let on = !BotWLaunchSettings.dev
                    UserDefaults.standard.set(on, forKey: BotWLaunchSettings.devKey)
                    model.message = BotWText(on ? "dev.on" : "dev.off")
                }
            statusLine.padding(.top, 4)
        }
    }

    private struct Status { var text: String; var symbol: String; var tint: Color }

    private var status: Status {
        if model.checking { return Status(text: tr("home.checking"), symbol: "arrow.triangle.2.circlepath", tint: .secondary) }
        if let busy = model.busy, !model.started, model.autoImporting || model.importProgress != nil {
            // the import running (started by itself or from the Game Files page), with its progress
            let p = model.importProgress
            let pct = p.map { " " + String(Int(min(100, $0.done * 100 / max(1, $0.total)))) + "%" } ?? ""
            return Status(text: tr("home.importing") + pct + " — " + busy, symbol: "arrow.down.circle", tint: .secondary)
        }
        if model.pending { return Status(text: tr("import.pending"), symbol: "exclamationmark.triangle", tint: .orange) }
        if model.game.ready { return Status(text: tr("home.ready"), symbol: "checkmark.circle", tint: .green) }
        return Status(text: tr("home.noFiles") + " — " + tr("home.noFilesNote"), symbol: "exclamationmark.triangle", tint: .orange)
    }

    private var statusLine: some View {
        let info = status
        return Label {
            Text(info.text)
                .foregroundStyle(info.tint == .green ? AnyShapeStyle(Color.green)
                                 : info.tint == .orange ? AnyShapeStyle(Color.orange) : AnyShapeStyle(.primary))
                .fixedSize(horizontal: false, vertical: true)
        } icon: {
            Image(systemName: info.symbol).foregroundStyle(info.tint)
        }
        .font(.body)
    }

    private var actions: some View {
        VStack(alignment: .trailing, spacing: 14) {
            HStack(spacing: 16) {
                primaryButton
                Button { path.append(.settings) } label: {
                    Label(tr("home.settings"), systemImage: "gearshape").padding(.horizontal, 8)
                }
                .buttonStyle(.bordered)
                .buttonBorderShape(.capsule)
                .controlSize(.extraLarge)
            }
            // the experience save, here as well as in Settings > Saves (user 2026-11-17: in the settings nobody finds it),
            // under the buttons where the title block leaves room (under the status line it made the bar taller); off by
            // default, so a first game starts at the beginning
            if model.game.ready { BotWExperienceToggle(settings: model.settings) }
        }
        .fixedSize()
    }

    private var primaryButton: some View {
        let ready = model.game.ready && !model.checking
        let busy = model.checking || model.busy != nil
        let enabled = !busy
        let title = !ready ? tr("home.prepare") : model.busy != nil ? tr("home.launching") : tr("home.start")
        return Button {
            if ready { model.start() } else { path.append(.files) }
        } label: {
            HStack(spacing: 10) {
                if busy { ProgressView().controlSize(.small) } else { Image(systemName: ready ? "play.fill" : "folder") }
                Text(title)
            }
            .font(.title3.bold())
            .foregroundStyle(enabled ? AnyShapeStyle(Color.black) : AnyShapeStyle(.secondary))
            .padding(.horizontal, 16)
            .frame(minWidth: 200)
        }
        .buttonStyle(.borderedProminent)
        .tint(enabled ? .white : nil)
        .buttonBorderShape(.capsule)
        .controlSize(.extraLarge)
        .disabled(!enabled)
    }
}

/// The home page's switch for the experience save (the same setting as Settings > Saves)
struct BotWExperienceToggle: View {
    @ObservedObject var settings: BotWLaunchSettings

    var body: some View {
        return Toggle(isOn: $settings.experienceSave) {
            Label(tr("home.experience"), systemImage: "sparkles").font(.body)
        }
        .toggleStyle(.switch)
        .fixedSize()
    }
}

/// The cover (ios/assets/Vision.xcassets/Cover), the SheikahVision key art without the Bokoblin since 2026-11-12 (user:
/// every build shows this one; the "FULL VR MOD" art is kept for videos in
/// docs/open-source/video-assets), filling its frame around
/// a focal point (KleptonGameLauncher.swift LauncherHeroImage.cropLayout); a neutral stand-in when it is missing
struct BotWHeroImage: View {
    static let focus = CGPoint(x: 0.45, y: 0)  // the top kept whole (the art's "FULL" was cut), the bottom gives way

    var body: some View {
        ZStack {
            Rectangle().fill(.black.opacity(0.18))
            if let image = UIImage(named: "Cover") {
                GeometryReader { geo in
                    let l = Self.cropLayout(image: image.size, frame: geo.size, focus: Self.focus)
                    Image(uiImage: image)
                        .resizable()
                        .frame(width: l.width, height: l.height)
                        .offset(x: l.x, y: l.y)
                }
                .clipped()
            } else {
                LinearGradient(colors: [.white.opacity(0.10), .white.opacity(0.03)], startPoint: .topLeading, endPoint: .bottomTrailing)
                Image(systemName: "photo").font(.system(size: 44)).foregroundStyle(.tertiary)
            }
        }
        .accessibilityHidden(true)
    }

    /// fill the frame, the focal point as near the middle as the image's edges allow
    static func cropLayout(image: CGSize, frame: CGSize, focus: CGPoint) -> (width: CGFloat, height: CGFloat, x: CGFloat, y: CGFloat) {
        guard image.width > 0, image.height > 0, frame.width > 0, frame.height > 0 else { return (frame.width, frame.height, 0, 0) }
        let scale = max(frame.width / image.width, frame.height / image.height)
        let w = image.width * scale, h = image.height * scale
        let x = min(0, max(frame.width - w, frame.width / 2 - focus.x * w))
        let y = min(0, max(frame.height - h, frame.height / 2 - focus.y * h))
        return (w, h, x, y)
    }
}

/// a settings row: an SF Symbol, a title and one line of detail (KleptonGameLauncherPages.swift LauncherIconRow)
struct BotWIconRow: View {
    let symbol: String
    let title: String
    let subtitle: String

    var body: some View {
        HStack(alignment: .top, spacing: 14) {
            Image(systemName: symbol).font(.title3).foregroundStyle(.secondary).frame(width: 32).accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 4) {
                Text(title).font(.headline)
                Text(subtitle).font(.subheadline).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
        .frame(minHeight: 44, alignment: .leading)
        .padding(.vertical, 4)
    }
}

struct BotWSettingsPage: View {
    @ObservedObject var settings: BotWLaunchSettings
    let page: BotWRoute?  // nil: the list; .picture / .controls / .language: that group
    @AppStorage(BotWLaunchSettings.devKey) private var dev = false

    // a list of rows (icon, title, one line of detail) leading to each group, as the shared Klepton launcher's
    // settings (KleptonGameLauncherPages.swift LauncherIconRow); user 2026-11-05
    var body: some View {
        switch page {
        case .picture: Form { pictureSection }
        case .controls: Form { controlsSection }
        case .language: Form { languageSection }
        default:
            List {
                row(.controls, "gamecontroller", tr("set.controls"), tr("set.controlsRow"))
                // developer settings only (five taps on the home page's version line)
                if dev {
                    row(.picture, "sparkles.tv", tr("set.picture"), tr("set.pictureRow"))
                    row(.language, "globe", tr("import.gameLanguage"), tr("set.languageRow"))
                }
                row(.saves, "externaldrive", tr("tab.saves"), tr("set.savesRow"))
                row(.files, "folder", tr("tab.files"), tr("set.filesRow"))
                row(.guide, "book", tr("tab.guide"), tr("set.guideRow"))
                row(.about, "info.circle", tr("tab.about"), tr("set.aboutRow"))
                Button(role: .destructive) { settings.reset() } label: {
                    BotWIconRow(symbol: "arrow.counterclockwise", title: tr("set.reset"), subtitle: tr("set.resetRow"))
                }
            }
        }
    }

    private func row(_ route: BotWRoute, _ symbol: String, _ title: String, _ subtitle: String) -> some View {
        NavigationLink(value: route) { BotWIconRow(symbol: symbol, title: title, subtitle: subtitle) }
    }

    @ViewBuilder private var pictureSection: some View {
            Section {
                Picker(tr("set.quality"), selection: $settings.quality) {
                    ForEach(BotWLaunchSettings.Quality.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)
                Text(settings.quality.detail).font(.callout).foregroundStyle(.secondary)
                Picker(tr("set.compositorResolution"), selection: $settings.compositorResolution) {
                    ForEach(BotWLaunchSettings.CompositorResolution.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)
                Text(tr("set.compositorResolutionNote")).font(.callout).foregroundStyle(.secondary)
                Toggle(tr("set.steady30"), isOn: $settings.steady30)
                Toggle(tr("set.fastUploads"), isOn: $settings.fastUploads)
                // collapsed by default: each of these can make distant detail shimmer more
                DisclosureGroup(tr("set.advanced")) {
                    advancedToggle(tr("set.noFxaa"), $settings.noFxaa)
                    advancedToggle(tr("set.noDof"), $settings.noDof)
                    advancedToggle(tr("set.sharpen"), $settings.sharpen)
                }
            } footer: { Text(tr("set.pictureFooter")) }
    }

    @ViewBuilder private var controlsSection: some View {
            Section {
                // the picture quality the player picks (user 2026-11-05): Standard or Smooth (the render scale by chip:
                // M5 1.5 / 1.25, M2 1.25 / 1.0); Sharp only with the developer settings
                // segmented pickers show no label of their own in a visionOS form: a title above each (user 2026-11-11)
                titled(tr("set.quality")) {
                    Picker(tr("set.quality"), selection: $settings.quality) {
                        ForEach(BotWLaunchSettings.Quality.allCases.filter { dev || $0 != .sharp }) { Text($0.title).tag($0) }
                    }
                    .pickerStyle(.segmented)
                }
                Text(settings.quality.detail).font(.callout).foregroundStyle(.secondary)
                // first or third person (BOTW_VR_FIRST_PERSON): third puts the eyes at the game's camera behind Link, an
                // early mode that people asked for (2026-11-10); the hands' motion controls are first person only
                titled(tr("set.view")) {
                    Picker(tr("set.view"), selection: $settings.firstPerson) {
                        Text(tr("set.view.first")).tag(true)
                        Text(tr("set.view.third")).tag(false)
                    }
                    .pickerStyle(.segmented)
                }
                if !settings.firstPerson {
                    Text(tr("set.view.thirdNote")).font(.callout).foregroundStyle(.secondary)
                }
                titled(tr("set.cutscenes")) {
                    Picker(tr("set.cutscenes"), selection: $settings.cutscenesOnScreen) {
                        Text(tr("set.cutscenes.auto")).tag(false)
                        Text(tr("set.cutscenes.screen")).tag(true)
                    }
                    .pickerStyle(.segmented)
                }
                Text(tr("set.cutscenesNote")).font(.callout).foregroundStyle(.secondary)
                Picker(tr("set.turn"), selection: $settings.turn) {
                    ForEach(BotWLaunchSettings.Turn.allCases) { Text($0.title).tag($0) }
                }
                Text(tr("set.turnNote")).font(.callout).foregroundStyle(.secondary)
                titled(tr("set.height")) {
                    Picker(tr("set.height"), selection: $settings.height) {
                        ForEach(BotWLaunchSettings.Height.allCases) { Text($0.title).tag($0) }
                    }
                    .pickerStyle(.segmented)
                }
                Text(tr("set.heightNote")).font(.callout).foregroundStyle(.secondary)
            }
            if dev {
                Section(tr("set.dev")) {
                    Toggle(tr("set.roomscale"), isOn: $settings.roomscale)
                    Toggle(tr("set.classic"), isOn: $settings.classicButtons)
                }
            }
    }

    @ViewBuilder private var languageSection: some View {
            Section {
                Picker(tr("import.gameLanguage"), selection: $settings.gameLanguage) {
                    ForEach(settings.availableLanguages, id: \.self) { Text(tr("import.lang." + String($0.suffix(2)))).tag($0) }
                }
                if !settings.gameRegion.isEmpty { Text(tr("import.region." + settings.gameRegion)).foregroundStyle(.secondary) }
            }
    }

    private func titled<Content: View>(_ title: String, @ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
            content().labelsHidden()
        }
    }

    private func advancedToggle(_ title: String, _ isOn: Binding<Bool>) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Toggle(title, isOn: isOn)
            Text(tr("set.shimmerNote")).font(.callout).foregroundStyle(.secondary)
        }
    }
}

struct BotWSavesPage: View {
    @ObservedObject var model: BotWLauncherModel
    @ObservedObject var settings: BotWLaunchSettings
    @State private var exporting = false
    @State private var importing = false
    @State private var exportDoc: BotWSaveFolder?

    var body: some View {
        let accounts = BotWSaves.accounts()
        Form {
            Section(tr("saves.current")) {
                if accounts.isEmpty {
                    Text(tr("saves.none")).foregroundStyle(.secondary)
                } else {
                    LabeledContent(tr("saves.folders"), value: tr("saves.count", accounts.count))
                    if let d = BotWSaves.latest() {
                        LabeledContent(tr("saves.latest"), value: d.formatted(date: .abbreviated, time: .shortened))
                    }
                }
            }
            Section {
                Toggle(tr("saves.experience"), isOn: $settings.experienceSave)
            } footer: {
                Text(tr("saves.experienceFooter"))
            }
            Section {
                Button(tr("saves.export")) {
                    exportDoc = try? BotWSaveFolder()
                    exporting = exportDoc != nil
                }
                .disabled(accounts.isEmpty)
                Button(tr("saves.import")) { importing = true }
            } footer: {
                Text(tr("saves.footer"))
            }
        }
        .padding(24)
        .fileExporter(isPresented: $exporting, document: exportDoc, contentType: .folder,
                      defaultFilename: tr("saves.exportName")) { result in
            if case .failure(let e) = result { model.message = BotWText("saves.exportFailed", e.localizedDescription) }
            else { model.message = BotWText("saves.exported") }
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.folder]) { result in
            guard case .success(let url) = result else { return }
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            do { model.message = try BotWSaves.importFolder(url) } catch { model.message = BotWText("importFailed", error.localizedDescription) }
        }
    }
}

struct BotWGamePage: View {
    @ObservedObject var model: BotWLauncherModel
    @State private var importing = false
    @State private var externalZip: URL?
    @State private var warnExternal = false
    @State private var confirmDelete = false
    @State private var confirmAbandon = false

    var body: some View {
        Form {
            Section(tr("files.result")) {
                if model.checking { ProgressView(tr("files.checking")) }
                else if let receipt = model.receipt {
                    Label(tr("files.complete"), systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                    Text("v\(receipt.titleVersion) · " + tr("import.region." + receipt.region))
                    Text(BotWGameImport.size(receipt.totalBytes))
                    if model.hasManifest { Button(tr("import.deep")) { model.maintenance("deep") } }
                    else { Text(tr("import.noManifest")).foregroundStyle(.secondary) }
                } else { Text(tr("home.noFiles")) }
                if model.hasGameFiles { Button(tr("import.delete"), role: .destructive) { confirmDelete = true } }
                if model.unfinished {
                    Text(tr("import.unfinished")).foregroundStyle(.orange)
                    Button(tr("import.abandon"), role: .destructive) { confirmAbandon = true }
                }
                Button(tr("files.recheck")) { model.check() }
            }
            .disabled(model.busy != nil)
            Section {
                Button(tr("import.check")) { model.inspectSources() }
                Button(tr("files.importButton")) { importing = true }
                if let source = model.sources.first {
                    Text(source.url.lastPathComponent).font(.headline)
                    Text(tr("import.summary", BotWGameImport.size(source.bytes), source.files.count, source.version, tr("import.region." + source.region)))
                    if let space = model.spaceText { Text(space) }
                    if source.hasDLC { Text(tr("import.dlc")) }
                    if model.sources.count > 1 {
                        Text(tr("import.onlyOne"))
                        ForEach(model.sources.dropFirst()) { Text($0.url.path).foregroundStyle(.secondary) }
                    }
                    Button(tr("import.begin")) { model.beginImport() }.buttonStyle(.borderedProminent).disabled(!model.spaceReady)
                }
                ForEach(Array(model.notices.enumerated()), id: \.offset) { Text($0.element).foregroundStyle(.orange) }
            } header: { Text(tr("files.import")) } footer: {
                VStack(alignment: .leading, spacing: 4) { Text(tr("import.location")); Text(tr("import.recommend")) }
            }
            .disabled(model.busy != nil || model.checking)
            if let busy = model.busy {
                Section {
                    ProgressView(busy)
                    if let progress = model.importProgress {
                        ProgressView(value: Double(progress.done), total: Double(max(1, progress.total)))
                        Text(tr("import.progress", Int(min(100, progress.done * 100 / max(1, progress.total))),
                                BotWGameImport.size(progress.done), BotWGameImport.size(progress.total),
                                BotWGameImport.size(UInt64(max(0, progress.bytesPerSecond))), Int(progress.secondsLeft)))
                    }
                    Text(tr("import.foreground"))
                    if model.cancellable { Button(tr("files.cancel")) { model.cancelImport() } }
                }
            }
        }
        .padding(24)
        .confirmationDialog(tr("import.external"), isPresented: $warnExternal, titleVisibility: .visible) {
            Button(tr("import.continue")) { if let url = externalZip { model.importGameZip(url) }; externalZip = nil }
            Button(tr("files.cancel"), role: .cancel) { externalZip = nil }
        }
        .confirmationDialog(tr("import.deleteConfirm"), isPresented: $confirmDelete, titleVisibility: .visible) {
            Button(tr("import.delete"), role: .destructive) { model.maintenance("delete") }
        }
        .confirmationDialog(tr("import.abandonConfirm"), isPresented: $confirmAbandon, titleVisibility: .visible) {
            Button(tr("import.abandon"), role: .destructive) { model.maintenance("abandon") }
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.folder, .zip]) { result in
            if case .success(let url) = result {
                if url.pathExtension.lowercased() == "zip", !url.standardizedFileURL.path.hasPrefix(BotWPaths.documents.path + "/") {
                    externalZip = url; warnExternal = true
                } else { model.importGame(url) }
            }
        }
    }
}

/// About (user 2026-11-11): the website, the source, the version, who it is built on, and that it is unofficial
struct BotWAboutPage: View {
    let version: String

    var body: some View {
        Form {
            Section {
                Link(destination: URL(string: "https://www.sheikahvision.com/")!) {
                    LabeledContent(tr("about.website"), value: "sheikahvision.com")
                }
                Link(destination: URL(string: "https://github.com/zlinoliver/BotW-SheikahVision")!) {
                    LabeledContent(tr("about.source"), value: "BotW-SheikahVision")
                }
                LabeledContent(tr("about.version"), value: version)
            }
            Section(tr("about.credits")) {
                Text(tr("about.credit.bettervr"))
                Text(tr("about.credit.cemu"))
                Text(tr("about.credit.recomp"))
            }
            Section { Text(tr("about.disclaimer")).foregroundStyle(.secondary) }
        }
    }
}

struct BotWGuidePage: View {

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                BotWGuideDiagram()
                VStack(alignment: .leading, spacing: 8) {
                    Text(tr("guide.tips")).font(.title3.weight(.semibold))
                    ForEach(["guide.tip1", "guide.tip2", "guide.tip3", "guide.tip4"], id: \.self) {
                        Text(tr($0)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    }
                }
                .padding(.horizontal, 8)
            }
            .padding(24)
        }
    }
}

// MARK: - entry points for main_ios.mm / window.mm

private var launcherWindow: UIWindow?

/// main_ios.mm, main thread, before the game starts: shows the launcher and returns once the player taps Start
@_cdecl("botw_launcher_run")
public func botw_launcher_run() {
    guard ProcessInfo.processInfo.environment["BOTW_LAUNCHER"] != "0" else { return }
    guard let scene = UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene })
        .first(where: { $0.session.role == .windowApplication }) else {
        NSLog("[launcher] no window scene, starting the game directly")
        return
    }
    let model = BotWLauncherModel()
    // the launcher's size, as the TotK app's (1280 x 800): the cover above, the info area under it
    scene.requestGeometryUpdate(.Vision(size: CGSize(width: 1280, height: 800)))
    let window = UIWindow(windowScene: scene)
    window.rootViewController = UIHostingController(rootView: BotWLauncherView(model: model))
    window.makeKeyAndVisible()
    launcherWindow = window
    NSLog("[launcher] shown")
    while !model.started {
        _ = CFRunLoopRunInMode(.defaultMode, 0.05, true)
    }
    // the SwiftUI view goes too, not only the window: its navigation stack's empty bar stayed in the game's window as
    // a grey strip at the bottom (device 2026-11-05)
    window.rootViewController = UIViewController()
    window.isHidden = true
    launcherWindow = nil
    // back to 16:9 before the game's window is made (window.mm asks for 1280 x 720 keeping the aspect, which kept the
    // launcher's 16:10: the 16:9 picture left the window's grey glass showing as a strip at the bottom, device
    // 2026-11-05)
    scene.requestGeometryUpdate(.Vision(size: CGSize(width: 1280, height: 720))) { error in
        NSLog("[launcher] 16:9 request failed: \(error)")
    }
    let until = Date().addingTimeInterval(0.3)
    while Date() < until { _ = CFRunLoopRunInMode(.defaultMode, 0.05, true) }
}

/// window.mm, once the game's window exists: a cover over the game's flat picture, which the window would otherwise
/// show until it closes (BotWImmersive.swift closes it once the game shows in the headset)
@_cdecl("botw_launcher_cover")
public func botw_launcher_cover(_ viewPtr: UnsafeMutableRawPointer) {
    let view = Unmanaged<UIView>.fromOpaque(viewPtr).takeUnretainedValue()
    let cover = UIHostingController(rootView:
        VStack(spacing: 16) {
            ProgressView()
            Text(tr("cover.title")).font(.title2)
            Text(tr("cover.note"))
                .font(.callout).foregroundStyle(.secondary).multilineTextAlignment(.center)
        }
        .padding(40)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(.regularMaterial))
    cover.view.frame = view.bounds
    cover.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    view.addSubview(cover.view)
    objc_setAssociatedObject(view, "botwCover", cover, .OBJC_ASSOCIATION_RETAIN)  // kept alive with the view
}
