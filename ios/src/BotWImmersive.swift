// VR on Apple Vision Pro, stage 3a: the game's two eye pictures on a stereo screen in an immersive space.
//
// The app stays SDL's UIKit app (its window keeps the game's own picture); with BOTW_VR=1 it also opens a SwiftUI
// ImmersiveSpace through UIHostingSceneDelegate (visionOS 26+). The space's CompositorLayer runs a render loop on its
// own thread: each headset frame it takes the newest picture set the game's renderer published
// (runtime/cemu_shims/latte_glue.mm, ios/src/vr_bridge.h) and draws a 16:9 screen fixed in the room, the left eye's
// view sampling the left picture and the right eye's the right one, with the HUD over both. The screen is placed once,
// in front of the head when the space opens. Stage 3b replaces the screen with the game's view drawn for the headset's
// own eye poses and fields of view.
//
// Stage 3b (BOTW_VR_MODE=view): the game renders each eye for the headset's own pose and frustum
// (runtime/src/game/vr_render.cpp). This loop publishes the predicted pose every frame (botw_vr_set_pose) and keeps the
// device anchor of each pose by its tag; a picture set comes back tagged with the pose it was rendered for, its eye
// images fill the views as they are, and that pose's anchor goes to the drawable so the compositor reprojects them to
// where the head is now. The HUD is a panel fixed in the room (BOTW_VR_HUD_M wide, 1.6 m; BOTW_VR_HUD_DISTANCE_M, 1.8 m).
// Frames without eye images (menus, loading) still go on the screen.
//
// BOTW_VR_FULL=1 / 0: fully immersive or mixed (the room around the screen); by default view mode is full and screen
// mode mixed. BOTW_VR_SCREEN_M: screen width (4 m),
// BOTW_VR_DISTANCE_M: its distance (3 m).
import ARKit
import CompositorServices
import CoreText
import GameController
import Metal
import SwiftUI
import UIKit
import simd

private func envFloat(_ name: String, _ dflt: Float) -> Float {
    guard let v = ProcessInfo.processInfo.environment[name], let f = Float(v) else { return dflt }
    return f
}

private extension Duration {
    var timeInterval: TimeInterval {
        let c = components
        return TimeInterval(c.seconds) + TimeInterval(c.attoseconds) * 1e-18
    }
}

final class BotWImmersiveDelegate: NSObject, UIHostingSceneDelegate {
    static let spaceID = "botw-vr"
    // view mode is VR: fully immersive unless BOTW_VR_FULL=0; screen mode keeps the room (mixed) unless BOTW_VR_FULL=1
    static var full: Bool {
        guard let v = ProcessInfo.processInfo.environment["BOTW_VR_FULL"] else { return botw_vr_view_mode() != 0 }
        return v == "1"
    }

    static var handsVisible: Bool {
        guard botw_vr_view_mode() != 0 else { return true }
        return ProcessInfo.processInfo.environment["BOTW_VR_HANDS"] == "1"
    }
    // Progressive immersion is not offered: a scene that declares it may only use the layered layout (one texture array,
    // device 2026-11-08: dedicated aborts as "Layer configuration not supported", layered aborts at the first present),
    // and this renderer draws each eye to its own texture (dedicated). It needs layered rendering first
    static var style: any ImmersionStyle { full ? .full : .mixed }

    static var rootScene: some Scene {
        ImmersiveSpace(id: spaceID) {
            CompositorLayer(configuration: BotWLayerConfiguration()) { layerRenderer in
                BotWCompositor.start(layerRenderer)
            }
        }
        .immersionStyle(selection: .constant(style), in: .mixed, .full)
        .persistentSystemOverlays(.hidden)
        // the player's real hands are hidden (Link's hands are drawn); BOTW_VR_HANDS=1 shows them, with what they hold
        // (the PS VR2 controllers, as far as the system cuts them out). Shown 2026-11-06 to 11-18 for the demo videos;
        // hidden in the dev app too since 2026-11-18 (user)
        .upperLimbVisibility(handsVisible ? .visible : .hidden)
    }
}

/// Called by runtime/src/host/window.mm once the game window exists (main thread). visionOS ignores the request while
/// the app is not in front (launched from the Mac with the headset off, say: seen on the device, 2026-11-01), so it is
/// asked again whenever the app becomes active and every few seconds until the compositor has started.
@_cdecl("botw_vr_open_immersive")
public func botw_vr_open_immersive() {
    requestImmersiveSpace(reason: "start")
    NotificationCenter.default.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil,
                                           queue: .main) { _ in requestImmersiveSpace(reason: "app active") }
    // closing the game's window or pressing the Digital Crown (the app goes to the background) ends the app outright,
    // so a test starts afresh without the force-quit dialog (user, 2026-11-01). BOTW_KEEP_RUNNING=1 keeps the old way
    if ProcessInfo.processInfo.environment["BOTW_KEEP_RUNNING"] != "1" {
        // only once the VR view has run: launched from the Mac while nobody wears the headset, the app is sent to the
        // background before it ever shows (device, 2026-11-02: it quit as it started)
        NotificationCenter.default.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil,
                                               queue: .main) { _ in
            guard BotWCompositor.everStarted else { return }
            NSLog("[vr] app sent to the background: quitting")
            _exit(0)  // not exit(): its C++ teardown ran while the game threads were still at work (signal 6)
        }
        // closing the window by hand (its ✕) before the game closes it ends the app as well; after that the window is
        // gone on purpose and the game goes on in the headset
        NotificationCenter.default.addObserver(forName: UIScene.didDisconnectNotification, object: nil,
                                               queue: .main) { note in
            guard let scene = note.object as? UIScene, scene.session.role == .windowApplication,
                  !BotWCompositor.windowClosed else { return }
            NSLog("[vr] the game window was closed: quitting")
            _exit(0)  // not exit(): its C++ teardown ran while the game threads were still at work (signal 6)
        }
    }
    Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { timer in
        if BotWCompositor.started { timer.invalidate(); return }
        requestImmersiveSpace(reason: "retry")
    }
}

private func requestImmersiveSpace(reason: String) {
    if BotWCompositor.started { return }
    guard let request = UISceneSessionActivationRequest(hostingDelegateClass: BotWImmersiveDelegate.self,
                                                        id: BotWImmersiveDelegate.spaceID) else {
        NSLog("[vr] no activation request for the immersive space")
        return
    }
    UIApplication.shared.activateSceneSession(for: request) { error in
        NSLog("[vr] opening the immersive space failed (\(reason)): \(error)")
    }
    NSLog("[vr] immersive space requested (\(BotWImmersiveDelegate.full ? "full" : "mixed"), hands \(BotWImmersiveDelegate.handsVisible ? "shown" : "hidden"), \(reason))")
}

struct BotWLayerConfiguration: CompositorLayerConfiguration {
    nonisolated(unsafe) private static var logged = false
    func makeConfiguration(capabilities: LayerRenderer.Capabilities, configuration: inout LayerRenderer.Configuration) {
        configuration.colorFormat = .bgra8Unorm_srgb
        configuration.depthFormat = .depth32Float
        // the compositor's own GPU work kept small: it shares the GPU with the game, and a little more of it dropped the
        // headset to 45 Hz (CAS sharpening, 2026-11-01). Foveation (BOTW_VR_FOVEATION=0 turns it off) and a render
        // quality below the default (BOTW_VR_QUALITY, 0.85 of it; Fable 5.1's analysis, 2026-11-02, had 0.6, and the
        // picture was "mosaic... very grainy" once the frame rate held, 2026-11-03: the eye images were sampled down once more
        // on their way to the display)
        let foveate = capabilities.supportsFoveation && ProcessInfo.processInfo.environment["BOTW_VR_FOVEATION"] != "0"
        configuration.isFoveationEnabled = foveate
        let layouts = capabilities.supportedLayouts(options: foveate ? [.foveationEnabled] : [])
        configuration.layout = layouts.contains(.dedicated) ? .dedicated : .layered
        if #available(visionOS 26.0, *) {
            // the launcher's "compositor resolution": System default sets BOTW_VR_QUALITY 1.0 (a factor of the
            // system's default quality, 0.52 on the device, 2026-11-04); High / Higher / Max set BOTW_VR_RENDER_QUALITY
            // 0.7 / 0.85 / 1.0, an absolute render quality ([0, 1], CompositorServices). The max render quality sizes
            // the drawable textures (memory); the layer's renderQuality is raised to it once the renderer starts
            let env = ProcessInfo.processInfo.environment
            let factor = env["BOTW_VR_QUALITY"].flatMap(Float.init) ?? 0.85
            let absolute = env["BOTW_VR_RENDER_QUALITY"].flatMap(Float.init)
            let quality = min(max(absolute ?? capabilities.defaultRenderQuality.rawValue * factor, 0.1), 1)
            configuration.maxRenderQuality = LayerRenderer.RenderQuality(rawValue: quality)
            if !Self.logged {
                Self.logged = true
                botw_vr_log("[vr] compositor quality " + (absolute.map { "absolute \($0)" } ?? "factor \(factor)")
                            + ": max render quality \(quality) (system default \(capabilities.defaultRenderQuality.rawValue)), "
                            + "foveation \(foveate)")
            }
        }
    }
}

@_silgen_name("botw_vr_player_ready")
private func botw_vr_player_ready() -> Int32  // vr_render.cpp: Link is in the world, in the view
@_silgen_name("botw_vr_crop")
private func botw_vr_crop(_ eye: Int32, _ out: UnsafeMutablePointer<Float>)  // vr_render.cpp: BOTW_VR_FOV per edge
@_silgen_name("botw_vr_view_now")
private func botw_vr_view_now() -> Int32  // vr_render.cpp: 0 while an event or a menu is on the screen in front
@_silgen_name("botw_vr_compiles")
private func botw_vr_compiles() -> Int32  // cemu_misc_shims.cpp: shaders and pipelines compiled so far
@_silgen_name("botw_vr_title_screen")
private func botw_vr_title_screen() -> Int32  // vr_render.cpp: the game's title menu is up

@_cdecl("botw_vr_window_closed")
public func botw_vr_window_closed() -> Int32 { BotWCompositor.windowClosed ? 1 : 0 }

@_silgen_name("botw_vr_bow_aiming")
private func botw_vr_bow_aiming() -> Int32  // vr_body.cpp: the bow drawn
@_silgen_name("botw_vr_throw_aiming")
private func botw_vr_throw_aiming() -> Int32  // vr_gesture.cpp: a throw being aimed (R held for it)
@_silgen_name("botw_vr_bow_dir_in_grip")
private func botw_vr_bow_dir_in_grip(_ out: UnsafeMutablePointer<Float>) -> Int32  // vr_body.cpp: the bow's -x, grip frame
@_silgen_name("botw_vr_bow_origin_in_grip")
private func botw_vr_bow_origin_in_grip(_ out: UnsafeMutablePointer<Float>) -> Int32  // vr_body.cpp: the arrow's launch point, grip frame
@_silgen_name("botw_vr_bow_flight")
private func botw_vr_bow_flight(_ out: UnsafeMutablePointer<Float>) -> Int32  // vr_bow.cpp: the drawn bow's flight

@_silgen_name("botw_vr_log")
private func botw_vr_log(_ line: UnsafePointer<CChar>)  // vr_render.cpp: a line in runtime.log

@_silgen_name("botw_main_window_closed")
private func botw_main_window_closed()  // MetalRenderer.cpp: nothing more drawn into the window

final class BotWCompositor {
    /// The window, once the game shows in the headset, is closed for good (the Klepton launchers' rule, porting manual
    /// 14 B: a window left in place sits in the view and took the controllers' presses when looked at, device
    /// 2026-11-01). The trigger is the compositor showing the game's pictures for BOTW_VR_CLOSE_WINDOW_S (3 s) in a
    /// row, not a timer; the renderer stops drawing into the window first. BOTW_VR_CLOSE_WINDOW_S=0 keeps it
    static var windowClosed = false  // main thread
    static func closeGameWindow() {
        guard !windowClosed else { return }
        windowClosed = true
        botw_main_window_closed()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {  // the renderer done with its last window frame
            for case let scene as UIWindowScene in UIApplication.shared.connectedScenes
            where scene.session.role == .windowApplication {
                NSLog("[vr] the game shows in the headset: closing its window")
                UIApplication.shared.requestSceneSessionDestruction(scene.session, options: nil) { error in
                    NSLog("[vr] closing the window failed: \(error)")
                }
            }
        }
    }
    /// View mode: the app's window stays in the immersive space, over the game (device, 2026-11-01: the side-by-side
    /// TV picture floating in front of the VR view; depth does not put the immersive picture in front of it), and
    /// visionOS has no way to hide it. While the space is up it shrinks to a small blank panel; back to 1280x720 after.
    /// What it shows does not matter meanwhile (the eye images come from the game's own buffers, not from the window).
    static func tuckGameWindow(_ tucked: Bool) {
        guard botw_vr_view_mode() != 0 else { return }
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes
        where scene.session.role == .windowApplication {
            guard let window = scene.windows.first, let view = window.rootViewController?.view else { continue }
            view.alpha = tucked ? 0 : 1
            // the whole window (the root view alone still showed the picture), and it takes no input: looked at, it took
            // the controllers' presses away from the game (device, 2026-11-01)
            for w in scene.windows { w.alpha = tucked ? 0 : 1; w.isUserInteractionEnabled = !tucked }
            let size = tucked ? CGSize(width: 320, height: 180) : CGSize(width: 1280, height: 720)
            scene.sizeRestrictions?.minimumSize = tucked ? size : CGSize(width: 640, height: 360)
            let geometry = UIWindowScene.GeometryPreferences.Vision(size: size)
            geometry.resizingRestrictions = .uniform
            scene.requestGeometryUpdate(geometry) { error in NSLog("[vr] window resize failed: \(error)") }
            NSLog("[vr] game window \(tucked ? "tucked away" : "restored")")
        }
    }

    static var started = false  // main thread: the immersive space is up (no more requests)
    static var everStarted = false  // main thread: it has been up at least once

    static func start(_ layerRenderer: LayerRenderer) {
        DispatchQueue.main.async { started = true; everStarted = true }
        let thread = Thread { BotWCompositor(layerRenderer).run() }
        thread.name = "BotW compositor"
        thread.qualityOfService = .userInteractive
        thread.start()
    }

    private let layerRenderer: LayerRenderer
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let viewPipeline: MTLRenderPipelineState  // view mode: an eye image filling its view
    private let hudPipeline: MTLRenderPipelineState   // view mode: the HUD panel, blended
    // view mode: the bow's aiming line. The arrow flies along the line from the drawing (right) hand through the bow
    // (left) hand (vr_bow.cpp launch), which the game's crosshair, in the middle of the flat HUD panel, cannot show
    // (device 2026-11-02: "no crosshair to be seen"); drawn from the controllers here while the bow is drawn, and hidden where it
    // goes into the game's world. It follows the arrow's flight (aimArc): straight while it keeps its speed, then
    // falling, as BetterVR's preview arc does. BOTW_VR_AIM_LINE=0 leaves it out
    private let aimPipeline: MTLRenderPipelineState
    // the arc's markers (BetterVR's preview): a red dot where the arrow starts to fall, a light blue one where it lands
    // (the first point of the arc behind the game's world, found on the GPU against its depth: mark_vertex)
    private let markPipeline: MTLRenderPipelineState
    private let aimDepthState: MTLDepthStencilState
    private let aimLine = ProcessInfo.processInfo.environment["BOTW_VR_AIM_LINE"] != "0"
    // the eye picture scaled up to the compositor's canvas with Catmull-Rom instead of bilinear (BOTW_VR_FILTER=bicubic)
    private let bicubic = ProcessInfo.processInfo.environment["BOTW_VR_FILTER"] == "bicubic"
    private let depthState: MTLDepthStencilState
    private let noDepthState: MTLDepthStencilState
    private let pictureDepthState: MTLDepthStencilState  // view mode: the eye image's depth, written everywhere
    // view mode: one depth for the whole eye image (BOTW_VR_PICTURE_DEPTH_M, 5 m). It must have one: drawn without depth
    // (left at the clear, infinitely far) the system showed nothing of it, the VR view stayed black while the game ran
    // (device, 2026-11-01; the menus, on a screen with depth, still showed). The system reprojects each picture to where
    // the eyes are when it is shown, the eyes' movement corrected as if everything were at this depth: at 1 m the world
    // (mostly much farther) shook when the head turned (device, 2026-11-01), so it is farther now. The real answer is
    // the game's own depth per pixel. 0 leaves it out
    private let pictureDepth = envFloat("BOTW_VR_PICTURE_DEPTH_M", 5.0)
    // view mode: each pixel's own depth, from the game's depth buffer (BOTW_VR_GAME_DEPTH=0: the one depth above
    // instead). The single depth corrects the eyes' movement right for one distance only, so the rest shook as the
    // head turned (device, 2026-11-01). Farther than BOTW_VR_DEPTH_MAX_M (200 m; the sky is at the far plane, 25 km)
    // counts as that far: a pixel left at depth 0 is not shown at all
    private let gameDepth = ProcessInfo.processInfo.environment["BOTW_VR_GAME_DEPTH"] != "0"
    private let maxDistance = envFloat("BOTW_VR_DEPTH_MAX_M", 200)
    private let viewMode = botw_vr_view_mode() != 0
    // view mode: contrast-adaptive sharpening of the eye images as they are scaled up to the display, BOTW_VR_SHARPEN
    // 0 (off, the default) .. 1. At 0.6 the headset's own loop fell from 90 to 45 Hz and the game from 47 to 35 fps
    // (device, 2026-11-01): five samples for each of the display's pixels, both eyes, 90 times a second, on the GPU the
    // game is using up. Sharpening belongs in the game's own picture, at its resolution (see the picture-quality notes)
    private let sharpness = envFloat("BOTW_VR_SHARPEN", 0)
    // view mode: our own rotational timewarp (Fable 5.1's option E, 2026-11-02). The game draws each eye
    // BOTW_VR_OVERSCAN (1.12) times wider than the view; each headset frame turns every display ray from the eye's
    // orientation now into the one the picture was drawn for and samples it there, depth included, and the system gets
    // the current pose. A head turn no longer uncovers an edge that only the next picture fills in, which the system's
    // own reprojection left (device 2026-11-02: corrections up to 1.5-4.9 degrees as a turn starts, stops or changes
    // speed: "turning the head still stutters a little"). BOTW_VR_OVERSCAN=1 turns it off (the system reprojects the picture as before)
    private let overscan = max(envFloat("BOTW_VR_OVERSCAN", 1.12), 1)
    private var warp: Bool { overscan > 1.001 }
    // BOTW_VR_FOV (one number, or outer,inner,top,bottom; 0.7-1, parsed in vr_render.cpp botw_vr_crop): the game draws a
    // narrower field (each edge's tangent times its number) at the same pixels, so the centre gets more pixels a degree;
    // the view beyond it, at the lenses' blurry edge, fades to black over BOTW_VR_EDGE_FADE (0.03 of the field) inside it
    private let crop: [SIMD4<Float>] = (0..<2).map { eye in
        var c = [Float](repeating: 1, count: 4)
        botw_vr_crop(Int32(eye), &c)
        return SIMD4<Float>(c[0], c[1], c[2], c[3])
    }
    private let edgeFade = min(max(envFloat("BOTW_VR_EDGE_FADE", 0.03), 0.005), 0.15)
    // BOTW_VR_EDGE_FILL: beyond the cropped field the picture's edge is carried on at this brightness instead of black
    // (user 2026-11-12: screenshots and recordings, which take the whole field the lenses do not show, had a black band
    // on one side); the fade at the crop goes down to it. 0 = black as before
    private let edgeFill = min(max(envFloat("BOTW_VR_EDGE_FILL", 0.5), 0), 1)
    // BOTW_VR_EDGE_RINGS=1 (developer settings): thin rings fixed to the display at 80, 85, 90, 95 and 98 % of each edge
    // (red, orange, yellow, green, blue), to find how much of each eye's view the lenses really show (plan A1,
    // quality notes section 18): the last ring seen on each side is how far that edge can be cropped
    private let edgeRings = ProcessInfo.processInfo.environment["BOTW_VR_EDGE_RINGS"] == "1"
    // view mode: the switch from the screen in front (title, loading, menus) into the game's view fades in over
    // BOTW_VR_FADE_S (0.5 s) instead of cutting
    private let fadeIn = Double(envFloat("BOTW_VR_FADE_S", 0.5))
    private var poseID: UInt32 = 0
    private var anchors: [UInt8: DeviceAnchor] = [:]  // by pose tag (the id's low 8 bits)
    // Timing (Fable 5.1, 2026-11-02): a picture is rendered for a pose and first shown some headset frames later; the
    // pose handed to the game is predicted for that moment (frames from publishing a pose to showing its picture,
    // averaged), so the system's reprojection only has the prediction error left to correct. Before, the pose was for
    // the publishing frame and the picture came 60-90 ms after it: a 6-9 degree correction in a fast head turn, the
    // uncovered edge refilled at the game's frame rate ("the picture refreshes bit by bit as I turn my head"). BOTW_VR_PREDICT=0: the old way
    private let predict = ProcessInfo.processInfo.environment["BOTW_VR_PREDICT"] != "0"
    private var publishedAt: [UInt8: Int] = [:]  // the compositor frame each pose tag was published in
    private var latencyFrames = 4.0               // frames from publishing to showing, averaged
    private var frameDuration = 1.0 / 90          // between presentations, averaged
    private var lastPresentation = 0.0
    private var lastSerial: UInt64 = 0
    private var repeats = 0
    private var heldPicture: BotWVRFrame?  // the picture on screen and how many headset frames it has shown
    private var loggedEyeSize = (0, 0)  // the eye picture's size last written to runtime.log (MetalFX changes it)
    private var heldFrames = 0
    // BOTW_VR_PACE: 2 (45 fps), 3 (a steady 30), 4, or auto (the launcher's default on the M2 since 2026-11-20; the M5
    // keeps 2, exactly as before, and a missing variable reads as 2 here and in vr_render.cpp). An M2 tester's
    // log (issue #12, v0.1.1: GPU 100% busy, 28 fps) had the pictures shown 3 and 4 times in no order (3x 68%, 4+x 21%,
    // 2x 11%): the judder. Auto (tallyPace) sets the game's pace too (botw_vr_set_pace, vr_render.cpp waits for it):
    // up a step when too many of 90 pictures missed their refresh (2 -> 3 at 40%, 3 -> 4 at 20%: a 3/4 mix judders more
    // than an even 22.5 fps), down a step when the game's own frame time fits the faster pace with 15% to spare for two
    // windows running, never sooner than a wait after the last switch (20 s, doubled when a step down was undone
    // within 30 s, up to 160 s; Fable's review 2026-11-20: a step back down by readiness alone swung every 8-9 s)
    private let paceAuto = ProcessInfo.processInfo.environment["BOTW_VR_PACE"] == "auto"
    // auto starts at 3 (2026-11-23, an M2 tester's v0.1.2 log: from 2 the first seconds in the world mixed 2, 3 and 4
    // shows until the step up at 30 s; the M2 never holds 45 in the world, a light scene can still step down later)
    private var minimumShows = ProcessInfo.processInfo.environment["BOTW_VR_PACE"] == "auto" ? 3
        : Int(ProcessInfo.processInfo.environment["BOTW_VR_PACE"] ?? "") ?? 2
    private var paceWindow = 0, paceMissed = 0, paceSwitches = 0, paceFits = 0
    private var paceSwitchedAt = Date(), paceDownAt = Date.distantPast, paceDwell = 20.0
    // the loop at 60 Hz or less (visionOS drops it to 45 when the headset is hot: a second M2 log, 2026-11-21, thermal
    // serious from the start and the loop at 45 Hz all along, every picture shown twice = 22.5 fps with the game at 26-32):
    // the pace then counts 1-2 of those frames (45 / 22.5 fps) instead of 2-4
    private var paceSlowLoop = false
    // the HUD layer of a stereo set that came without one: the last HUD kept on for up to BOTW_VR_HUD_HOLD pictures
    // (0, off by default: the M2 recording's missing minimap was the panel sliding out of view, Fable's review; and the
    // held texture is the one the set ring writes three sets later). The counts go to runtime.log with the timing line
    private let hudHold = max(Int(ProcessInfo.processInfo.environment["BOTW_VR_HUD_HOLD"] ?? "") ?? 0, 0)
    private var lastHud: MTLTexture?  // held strongly: the set's texture can be replaced on the renderer's side
    private var hudSerial: UInt64 = 0, hudGap = 0
    private var hudPictures = 0, hudMissing = 0, hudGaps = 0, hudLongest = 0, hudBridged = 0
    private var repeatCounts = [0, 0, 0, 0]       // pictures shown once, twice, three times, four or more
    private var correctionSum = 0.0, correctionMax = 0.0, correctionN = 0
    private final class GPUStats: @unchecked Sendable {  // the compositor's command buffers, from their completion
        let lock = NSLock()
        var late = 0, n = 0
        var queueSum = 0.0, queueMax = 0.0, runSum = 0.0
    }
    private let gpuStats = GPUStats()
    private var originFromHud: simd_float4x4?
    private var hudFollowing = false  // the HUD panel gliding back in front of the view
    private let hudWidth = envFloat("BOTW_VR_HUD_M", 1.6)
    private let hudDistance = envFloat("BOTW_VR_HUD_DISTANCE_M", 1.8)
    // the HUD panel follows once the head looks this far from it (BOTW_VR_HUD_FOLLOW_DEG, 30), gliding a share of the way
    // each frame (BOTW_VR_HUD_FOLLOW_K, 0.2). The launcher gives the M2 12 and 0.1 (2026-11-20: in an M2 tester's
    // recording the minimap slid out of the view as the head turned, then jumped back, read as a flicker); the M5 keeps these
    private let hudFollowAngle = envFloat("BOTW_VR_HUD_FOLLOW_DEG", 30) * .pi / 180
    private let hudFollowK = min(max(envFloat("BOTW_VR_HUD_FOLLOW_K", 0.2), 0.02), 1)
    private let arSession = ARKitSession()
    private var worldTracking = WorldTrackingProvider()  // replaced when the head stays untracked (restartTracking)
    private var untrackedFrames = 0
    private var restartingTracking = false
    // the PS VR2 Sense controllers' poses (visionOS 26 accessory tracking; not in the simulator): a provider for the
    // controllers connected so far, replaced when that changes (visionOS 27 can update it in place)
    private var accessoryTracking: AccessoryTrackingProvider?
    private var trackedControllers: [ObjectIdentifier] = []
    private var trackedAccessories: [Accessory] = []
    private var originFromScreen: simd_float4x4?
    private var screenWasFlat = false
    private let screenWidth = envFloat("BOTW_VR_SCREEN_M", 4.0)
    private let screenDistance = envFloat("BOTW_VR_DISTANCE_M", 3.0)
    private var frames = 0
    // view mode: the first eye images after a load show the world before its shaders and textures are ready (bars of
    // colour across the sky, device 2026-11-01), so for BOTW_VR_SETTLE_S (1.5 s) after the pictures turn into eye
    // images the view stays black
    private let settle = Double(envFloat("BOTW_VR_SETTLE_S", 1.5))
    private var viewSince: Date?
    private var viewFrames = 0  // headset frames in a row showing the game's eye images
    // The loading panel: drawn here, in the immersive space, in place of the game's pictures (not over them: while it
    // shows they are not drawn at all, and it is no window, nothing a player can move aside). It replaces what the game
    // shows on its way in, which looked like bugs (user 2026-11-02: "the in-between picture... users will think it is a bug"): while it boots
    // (its 2D boot screens, until its title menu is up), on its loading screen (LoadingWeapon_00, also for later loads
    // such as fast travel) and as its view first shows (the first frames, colour bars while shaders compile). The
    // title menu itself shows as before: the player picks the save (user: no automatic continue)
    private var bootDone = false           // the title menu (or the game's view) has been up once
    private var titleWas = false
    private var entering = false           // the title menu closed (a save chosen), the game's view not shown yet
    private let loadingStart = Date()
    private var phaseStart = Date()        // the current loading phase began
    private var phase = 0                  // 0 none, 1 booting, 2 loading screen, 3 entering the view
    // While the game loads it draws only a flat picture (its loading screen, no eye images: picture.stereo 0); the
    // panel stands in for that, and once the view is back it stays until the view has shown for `settle` and no
    // shader has been compiled for 1.5 s (at most 25 s), then the view fades in. The game's own loading-screen flag
    // came up for a frame as the pause menu closed and missed whole loading screens (device 2026-11-02: the panel
    // flashed in the menu, and went while "the crosshair handling" and a black screen still showed); a menu or an event keeps the
    // eye images (stereo), so neither brings the panel
    // the view no longer waits for the shaders to settle (user 2026-11-12: the spinner only before the title menu, never
    // after; a black wait over a playing game broke the experience): the game's own loading screens show, then its view
    private var needSettle = false
    private let settleMax = Double(envFloat("BOTW_VR_SETTLE_MAX", 4))
    private var settleFrom = Date()
    private var flatSince: Date?
    private var enteredAt = Date.distantPast
    // the first way into the game is done: from then on the game's own loading screens, black frames and menus show as
    // they are (user 2026-11-03: the panel only at the very start, not for menus, cutscenes, fast travel or shrines)
    private var started = false
    private var compiles: Int32 = -1
    private var compilesChangedAt = Date()
    private var loadingTexture: MTLTexture?
    private var loadingDrawnAt = Date.distantPast
    private var loadingProgress: Float = 0
    private let closeWindowAfter = Double(envFloat("BOTW_VR_CLOSE_WINDOW_S", 3))

    private init(_ layerRenderer: LayerRenderer) {
        self.layerRenderer = layerRenderer
        device = layerRenderer.device
        queue = device.makeCommandQueue()!
        let library = try! device.makeLibrary(source: Self.shaderSource, options: nil)
        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = library.makeFunction(name: "screen_vertex")
        desc.fragmentFunction = library.makeFunction(name: "screen_fragment")
        desc.colorAttachments[0].pixelFormat = layerRenderer.configuration.colorFormat
        desc.depthAttachmentPixelFormat = layerRenderer.configuration.depthFormat
        pipeline = try! device.makeRenderPipelineState(descriptor: desc)
        desc.vertexFunction = library.makeFunction(name: "view_vertex")
        desc.fragmentFunction = library.makeFunction(name: "view_fragment")
        viewPipeline = try! device.makeRenderPipelineState(descriptor: desc)
        desc.vertexFunction = library.makeFunction(name: "screen_vertex")
        desc.fragmentFunction = library.makeFunction(name: "hud_fragment")
        desc.colorAttachments[0].isBlendingEnabled = true
        desc.colorAttachments[0].sourceRGBBlendFactor = .one
        desc.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
        desc.colorAttachments[0].sourceAlphaBlendFactor = .one
        desc.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
        hudPipeline = try! device.makeRenderPipelineState(descriptor: desc)
        desc.vertexFunction = library.makeFunction(name: "aim_vertex")
        desc.fragmentFunction = library.makeFunction(name: "aim_fragment")
        aimPipeline = try! device.makeRenderPipelineState(descriptor: desc)
        desc.vertexFunction = library.makeFunction(name: "mark_vertex")
        desc.fragmentFunction = library.makeFunction(name: "mark_fragment")
        markPipeline = try! device.makeRenderPipelineState(descriptor: desc)
        noDepthState = device.makeDepthStencilState(descriptor: MTLDepthStencilDescriptor())!
        let aimDepth = MTLDepthStencilDescriptor()
        aimDepth.depthCompareFunction = .greater  // hidden behind the game's world (its depth is in the buffer)
        aimDepth.isDepthWriteEnabled = false
        aimDepthState = device.makeDepthStencilState(descriptor: aimDepth)!
        let depth = MTLDepthStencilDescriptor()
        depth.depthCompareFunction = .greater  // the compositor's projections are reverse-Z (clear 0 = far)
        depth.isDepthWriteEnabled = true
        depthState = device.makeDepthStencilState(descriptor: depth)!
        depth.depthCompareFunction = .always
        pictureDepthState = device.makeDepthStencilState(descriptor: depth)!
        if paceAuto { botw_vr_set_pace(Int32(minimumShows)) }  // a new renderer (the space reopened) starts both at 3
        var quality = "default"
        if #available(visionOS 26.0, *) {
            layerRenderer.renderQuality = layerRenderer.configuration.maxRenderQuality
            quality = String(format: "%.2f", layerRenderer.renderQuality.rawValue)
        }
        // in runtime.log too (the headset's log): the render quality and the one overscan both the game's widened
        // projection (the tangents it is handed) and the warp use
        let line = "[vr] compositor: layout \(layerRenderer.configuration.layout == .layered ? "layered" : "dedicated"), "
            + "screen \(screenWidth) m at \(screenDistance) m, mode \(viewMode ? "view" : "screen"), "
            + "foveation \(layerRenderer.configuration.isFoveationEnabled), render quality \(quality), "
            + "overscan \(overscan) (warp \(warp ? "on" : "off")), field left eye \(crop[0]), right eye \(crop[1]), edge fade \(edgeFade), edge rings \(edgeRings ? "on" : "off")"
        NSLog("%@", line)
        botw_vr_log(line)
    }

    private func run() {
        Task {  // what ARKit says about the session (authorization, provider states, errors): the device log
            for await event in arSession.events { NSLog("[vr] ARKit: \(event)") }
        }
        Task {
            await runProviders()
            await trackControllers()
            for await _ in NotificationCenter.default.notifications(named: .GCControllerDidConnect) { await trackControllers() }
        }
        while true {
            switch layerRenderer.state {
            case .invalidated:
                NSLog("[vr] compositor stopped after \(frames) frames")
                arSession.stop()
                DispatchQueue.main.async {
                    BotWCompositor.started = false  // asked for again (app active, retries)
                    BotWCompositor.tuckGameWindow(false)
                }
                return
            case .paused:
                layerRenderer.waitUntilRunning()
            default:
                autoreleasepool { renderFrame() }
            }
        }
    }

    private var loggedDrawable = false
    private var loggedCaptures = 0
    private func renderFrame() {
        guard let frame = layerRenderer.queryNextFrame() else { return }
        frame.startUpdate()
        frame.endUpdate()
        guard let timing = frame.predictTiming() else { return }
        LayerRenderer.Clock().wait(until: timing.optimalInputTime)
        // the headset's own drawable, and while a recording, a screenshot or AirPlay runs, a capture one (visionOS 26).
        // Only the first was drawn, so the system filled the capture's wider view from the left eye's, a black band
        // down its right side (item 96; user 2026-11-13, every recording and screenshot)
        let drawables = frame.queryDrawables()
        guard let drawable = drawables.first(where: { $0.target == .builtIn }) ?? drawables.first else { return }
        let captures = drawables.filter { $0.target == .capture }
        frame.startSubmission()
        if !loggedDrawable, let color = drawable.colorTextures.first {
            // once: the size the compositor's render quality gave the eye textures
            loggedDrawable = true
            var quality = "default"
            if #available(visionOS 26.0, *) {
                quality = String(format: "%.3f (max %.3f)", layerRenderer.renderQuality.rawValue,
                                 layerRenderer.configuration.maxRenderQuality.rawValue)
            }
            botw_vr_log("[vr] drawable: \(drawable.colorTextures.count) color texture(s) \(color.width)x\(color.height)"
                        + "x\(color.arrayLength), \(drawable.views.count) views, render quality \(quality)")
        }

        let presentation = LayerRenderer.Clock.Instant.epoch.duration(to: drawable.frameTiming.presentationTime).timeInterval
        if lastPresentation > 0, presentation > lastPresentation, presentation - lastPresentation < 0.05 {
            frameDuration += 0.05 * (presentation - lastPresentation - frameDuration)
        }
        lastPresentation = presentation
        let anchor = worldTracking.queryDeviceAnchor(atTimestamp: presentation)
        // no head pose for a second: start a fresh world tracking provider (seen on the device, 2026-11-01: the space
        // opened long after the provider was run, and the head stayed untracked)
        untrackedFrames = anchor == nil ? untrackedFrames + 1 : 0
        if untrackedFrames > 90 && !restartingTracking {
            restartingTracking = true
            untrackedFrames = 0
            NSLog("[vr] head untracked for 90 frames (provider \(worldTracking.state)), restarting world tracking")
            Task {
                worldTracking = WorldTrackingProvider()
                await runProviders()
                restartingTracking = false
            }
        }
        drawable.deviceAnchor = anchor
        let originFromDevice = anchor?.originFromAnchorTransform ?? matrix_identity_float4x4
        if originFromScreen == nil, anchor != nil {
            originFromScreen = placeScreen(originFromDevice, distance: screenDistance)
            originFromHud = placeScreen(originFromDevice, distance: hudDistance)
        }
        // the HUD panel follows lazily (device 2026-11-05: the hearts, the minimap and the scope sat low on the right once
        // the head had turned away from where the panel was placed): when the head looks more than 30 degrees away
        // from it, or is 30 cm higher or lower, the panel glides back in front (about a fifth of the way each frame)
        if viewMode, anchor != nil, let current = originFromHud {
            let target = placeScreen(originFromDevice, distance: hudDistance)
            let cf = simd_normalize(SIMD3<Float>(-current.columns.2.x, 0, -current.columns.2.z))
            let tf = simd_normalize(SIMD3<Float>(-target.columns.2.x, 0, -target.columns.2.z))
            let angle = acos(min(max(simd_dot(cf, tf), -1), 1))
            let dy = abs(current.columns.3.y - target.columns.3.y)
            if !hudFollowing, angle > hudFollowAngle || dy > 0.3 {
                hudFollowing = true
                NSLog("[vr] HUD panel follows the view (%.0f deg, %.2f m)", angle * 180 / .pi, dy)
            }
            if hudFollowing {
                let k = hudFollowK
                var f = simd_normalize(cf + (tf - cf) * k)
                if !f.x.isFinite { f = tf }
                let right = simd_normalize(simd_cross(f, SIMD3<Float>(0, 1, 0)))
                let head = SIMD3<Float>(originFromDevice.columns.3.x, originFromDevice.columns.3.y, originFromDevice.columns.3.z)
                let cy = current.columns.3.y + (target.columns.3.y - current.columns.3.y) * k
                var center = head + f * hudDistance
                center.y = cy
                originFromHud = simd_float4x4(SIMD4<Float>(right, 0), SIMD4<Float>(0, 1, 0, 0), SIMD4<Float>(-f, 0),
                                              SIMD4<Float>(center, 1))
                if angle < 2 * .pi / 180, dy < 0.02 { hudFollowing = false }
            }
        }
        if viewMode, let anchor { publishPose(anchor, drawable, presentation: presentation) }

        let commandBuffer = queue.makeCommandBuffer()!
        var picture = BotWVRFrame()
        let havePicture = botw_vr_latest_frame(&picture) != 0
        // each game picture (paced to every second headset frame, BOTW_VR_PACE) shows at least that many refreshes: one
        // finishing right as this loop samples showed once and its neighbour three times (device 2026-11-02, about 1 in 7
        // pictures, "shown 1x 66 2x 316 3x 67"), so a newer picture waits while the one on screen has shown only once.
        // Only the very next set is made to wait: the game writes the set after it meanwhile, never the held one (three
        // sets in turn, latte_glue.mm)
        if havePicture, picture.stereo != 0, let held = heldPicture, held.stereo != 0,
           picture.serial == held.serial + 1, heldFrames < minimumShows {
            picture = held
        }
        if havePicture {
            if picture.serial == heldPicture?.serial {
                heldFrames += 1
            } else {
                if let old = heldPicture { tallyPace(old: old, new: picture) }
                heldPicture = picture; heldFrames = 1
            }
        }
        if havePicture, picture.stereo != 0, let eye = texture(picture.eye.0), (eye.width, eye.height) != loggedEyeSize {
            loggedEyeSize = (eye.width, eye.height)
            botw_vr_log("[vr] eye picture \(eye.width)x\(eye.height)")
        }
        // a flat picture (the scope, a loading screen) comes up: put the screen in front of the head as it is now, not
        // where it was when the space opened (device 2026-11-03: the scope sat low and could not be brought ahead)
        let flatPicture = viewMode && havePicture && picture.stereo == 0
        if flatPicture && !screenWasFlat, anchor != nil { originFromScreen = placeScreen(originFromDevice, distance: screenDistance) }
        if havePicture { screenWasFlat = flatPicture }
        if havePicture, let event = botw_vr_shared_event() {
            // the set's pictures are written on the game renderer's queue: wait until the GPU has finished them
            commandBuffer.encodeWaitForEvent(Unmanaged<MTLSharedEvent>.fromOpaque(event).takeUnretainedValue(),
                                             value: picture.serial)
        }
        // view mode: the eye images were rendered for an earlier pose; present them with that pose's anchor
        let eyeImages = viewMode && havePicture && picture.stereo != 0 && (picture.pose_tag & 0x100) != 0
        if !eyeImages { viewSince = nil } else if viewSince == nil { viewSince = Date() }
        let compiled = botw_vr_compiles()
        if compiled != compiles { compiles = compiled; compilesChangedAt = Date() }
        let shown = Date().timeIntervalSince(viewSince ?? .distantFuture)
        let title = botw_vr_title_screen() != 0
        // a flat picture for 0.3 s in view mode, not the title menu: the game is loading
        if viewMode && havePicture && picture.stereo == 0 && !title && botw_vr_view_now() != 0 {  // not an event's TV picture
            if flatSince == nil { flatSince = Date() }
        } else {
            flatSince = nil
        }
        let flatLoading = Date().timeIntervalSince(flatSince ?? .distantFuture) > 0.3
        // only on the first way in: once started, nothing brings the panel back, not even a flat screen right after
        // it (user 2026-11-05: the spinner in play, at quick menus and scene changes, broke the game's feel)
        // the title menu's backdrop is in the view as well: only Link in the world counts as the way in being done
        let ready = botw_vr_player_ready() != 0 && !title
        if !eyeImages || !ready { settleFrom = Date() }
        // at most BOTW_VR_SETTLE_MAX (4 s) once Link is in the world: with the shader caches cold (after an install or an
        // update) the shaders kept compiling and the panel stood black for 25 s with the game already playing and heard
        // (device 2026-11-12); a few seconds of hitching in the view beat that
        let settling = eyeImages && needSettle && !title && Date().timeIntervalSince(settleFrom) < settleMax &&
            (!ready || shown < settle || Date().timeIntervalSince(compilesChangedAt) < 1.5)
        if eyeImages && ready && !started {
            started = true
            enteredAt = Date()
            NSLog("[vr] in the game's view after %.1f s (%.1f s of it settling), %d shaders and pipelines",
                  Date().timeIntervalSince(loadingStart), shown, compiled)
        }
        let viewPicture = eyeImages && !settling
        // any picture counts (the title and loading screens show on the screen in front): the window, now a cover
        // (BotWLauncher.swift), stands in front of that screen
        if havePicture && !settling { viewFrames += 1 } else { viewFrames = 0 }
        if !bootDone, botw_vr_title_screen() != 0 || viewPicture || Date().timeIntervalSince(loadingStart) > 180 {
            bootDone = true
            NSLog("[vr] start-up: the game is up after %.1f s", Date().timeIntervalSince(loadingStart))
        }
        // a save chosen: the title menu closes and the game loads the world, drawing in 2D meanwhile (on the Mac a
        // short load shows only a fade, no loading screen), until its view first shows
        if titleWas && !title { entering = true; phaseStart = Date() }
        // an event on the screen in front ends it too: a new game opens with one for most of a minute, and the panel
        // stood over it with only the voice to hear (device 2026-11-09)
        let eventOnScreen = havePicture && botw_vr_view_now() == 0
        if title || eyeImages || eventOnScreen || Date().timeIntervalSince(phaseStart) > 180 { entering = false }
        titleWas = title
        // the spinner only while the game starts, until its title menu is up (user 2026-11-12); after that the game's own
        // pictures, loading screens included, as everywhere else
        let newPhase = !viewMode || started ? 0 : !bootDone ? 1 : 0
        if newPhase != phase {
            if newPhase != 0 && (phase == 0 || newPhase == 1) { phaseStart = Date(); loadingProgress = 0 }
            phase = newPhase
        }
        let loading = phase != 0
        // the 2D window goes as soon as the loading panel is up, else once the game shows for a while
        if loading, frames == 90 { DispatchQueue.main.async { BotWCompositor.closeGameWindow() } }
        if closeWindowAfter > 0, viewFrames == Int(closeWindowAfter * 90) {
            DispatchQueue.main.async { BotWCompositor.closeGameWindow() }
        }
        if loading { updateLoadingPanel() }
        if havePicture {  // how long a picture takes to show, and how many headset frames each one stays
            if picture.serial != lastSerial {
                repeatCounts[min(repeats, 3)] += 1
                repeats = 0
                lastSerial = picture.serial
                if eyeImages, let at = publishedAt[UInt8(truncatingIfNeeded: picture.pose_tag)] {
                    latencyFrames += 0.1 * (Double(frames - at) - latencyFrames)
                }
            } else {
                repeats += 1
            }
        }
        // the HUD over the view: this set's, or the last one for a few pictures when a set came without it (above)
        var viewHud: MTLTexture? = nil
        if viewPicture {
            if picture.serial != hudSerial {
                hudSerial = picture.serial
                hudPictures += 1
                if let hud = texture(picture.hud) {
                    if hudGap > 0 { hudLongest = max(hudLongest, hudGap) }
                    hudGap = 0
                    if hudHold > 0 { lastHud = hud }  // nothing kept unless the hold is on (the M5 as before)
                } else {
                    if hudGap == 0 { hudGaps += 1 }
                    hudGap += 1
                    hudMissing += 1
                    if hudGap <= hudHold, lastHud != nil { hudBridged += 1 }
                }
            }
            viewHud = texture(picture.hud) ?? (hudHold > 0 && hudGap <= hudHold ? lastHud : nil)
        } else if havePicture && picture.stereo == 0 {
            lastHud = nil  // a menu or loading screen: no HUD carried over into the next view
            hudGap = 0
        }
        var shownFromDevice = originFromDevice
        var renderedFromDevice: simd_float4x4?  // the head the picture was drawn for (the warp's source)
        if viewPicture, let rendered = anchors[UInt8(truncatingIfNeeded: picture.pose_tag)] {
            renderedFromDevice = rendered.originFromAnchorTransform
            if !warp || anchor == nil {  // else the system gets the current pose (the warp turns the picture to it)
                drawable.deviceAnchor = rendered
                shownFromDevice = rendered.originFromAnchorTransform
            }
            if let anchor {  // the turn the system still has to correct: rendered view direction against the real one
                let a = -rendered.originFromAnchorTransform.columns.2, b = -anchor.originFromAnchorTransform.columns.2
                let c = Double(acos(min(max(simd_dot(simd_normalize(SIMD3(a.x, a.y, a.z)),
                                                     simd_normalize(SIMD3(b.x, b.y, b.z))), -1), 1))) * 180 / .pi
                correctionSum += c
                correctionMax = max(correctionMax, c)
                correctionN += 1
            }
        }
        /// one drawable's views; projectionOf gives each view's projection (the capture drawable takes the left eye's)
        func encodeViews(_ d: LayerRenderer.Drawable, projectionOf: (Int) -> simd_float4x4) {
            for (index, view) in d.views.enumerated() {
                let map = view.textureMap
                let pass = MTLRenderPassDescriptor()
                if layerRenderer.configuration.isFoveationEnabled, index < d.rasterizationRateMaps.count {
                    pass.rasterizationRateMap = d.rasterizationRateMaps[index]  // foveated drawables need their map
                }
                pass.colorAttachments[0].texture = d.colorTextures[map.textureIndex]
                pass.colorAttachments[0].slice = map.sliceIndex
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: BotWImmersiveDelegate.full ? 1 : 0)
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = d.depthTextures[map.textureIndex]
                pass.depthAttachment.slice = map.sliceIndex
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = 0
                pass.depthAttachment.storeAction = .store
                let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setViewports([map.viewport])
                if loading {
                    if let originFromScreen, let panel = loadingTexture {
                        let viewMatrix = (originFromDevice * view.transform).inverse
                        let width: Float = 1.4  // the spinner about 7 degrees across at the screen's distance, the text below it
                        var uniforms = ScreenUniforms(
                            clipFromScreen: projectionOf(index) * viewMatrix * originFromScreen,
                            size: SIMD2<Float>(width, width * Float(panel.height) / Float(panel.width)), hasHud: 1)
                        encoder.setRenderPipelineState(hudPipeline)
                        encoder.setDepthStencilState(depthState)  // in front of nothing: written so the panel shows
                        encoder.setVertexBytes(&uniforms, length: MemoryLayout<ScreenUniforms>.stride, index: 0)
                        encoder.setFragmentTexture(panel, index: 1)
                        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    }
                } else if viewPicture, let eyeTexture = texture(index == 1 ? picture.eye.1 : picture.eye.0) {
                    encoder.setRenderPipelineState(viewPipeline)
                    encoder.setDepthStencilState(pictureDepthState)
                    // reverse-Z: a point d metres ahead lands at depth -p22 + p32 / d
                    let projection = projectionOf(index)
                    let a = projection.columns.2.z, b = projection.columns.3.z
                    let eyeDepth = gameDepth ? texture(index == 1 ? picture.depth.1 : picture.depth.0) : nil
                    // the view's edges (tangents) and the picture's (the overscan wider), and the turn from the eye now
                    // to the eye the picture was drawn for
                    let edges = SIMD4<Float>((projection.columns.2.x - 1) / projection.columns.0.x,
                                             (projection.columns.2.x + 1) / projection.columns.0.x,
                                             (projection.columns.2.y + 1) / projection.columns.1.y,
                                             (projection.columns.2.y - 1) / projection.columns.1.y)
                    var turn = matrix_identity_float4x4
                    // each eye from the pose it was drawn with: the right eye can be a pose behind the left
                    // (vr_render.cpp botw_vr_latch_right_pose; device 2026-11-19, the right eye shook)
                    let eyeTag = index == 1 ? picture.pose_tag_eye.1 : picture.pose_tag_eye.0
                    let eyeRendered = (eyeTag & 0x100) != 0
                        ? anchors[UInt8(truncatingIfNeeded: eyeTag)]?.originFromAnchorTransform ?? renderedFromDevice
                        : renderedFromDevice
                    if warp, let eyeRendered {
                        turn = (eyeRendered * view.transform).inverse * (shownFromDevice * view.transform)
                    }
                    let fade = Float(min(max(Date().timeIntervalSince(enteredAt), 0) / max(fadeIn, 0.01), 1))
                    var params = ViewParams(
                        sharpness: sharpness, constDepth: pictureDepth > 0 ? -a + b / pictureDepth : 0,
                        useDepth: eyeDepth != nil ? 1 : 0, projA: a, projB: b,
                        near: picture.depth_range.0, far: picture.depth_range.1, zScale: picture.depth_range.2,
                        zOffset: picture.depth_range.3, maxDistance: maxDistance, fade: fade, bicubic: bicubic ? 1 : 0, fov: crop[min(index, 1)], edgeRings: edgeRings ? 1 : 0, edgeFade: edgeFade, edgeFill: edgeFill,
                        viewEdges: edges, pictureEdges: edges * (warp ? overscan : 1) * crop[min(index, 1)],
                        turn0: turn.columns.0, turn1: turn.columns.1, turn2: turn.columns.2)
                    encoder.setFragmentBytes(&params, length: MemoryLayout<ViewParams>.stride, index: 0)
                    encoder.setFragmentTexture(eyeTexture, index: 0)
                    encoder.setFragmentTexture(eyeDepth ?? eyeTexture, index: 1)
                    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                    // a throw being aimed: only the ring where the view meets the world (the throw goes along the view
                    // from above the eyes, vr_aim.cpp); a line from there would stand in the middle of the view
                    let throwAim = aimLine && botw_vr_throw_aiming() != 0 ? throwRay(shownFromDevice) : nil
                    if aimLine, let aim = throwAim ?? aimRay(at: presentation) {
                        let viewMatrix = (shownFromDevice * view.transform).inverse
                        let eye = (shownFromDevice * view.transform).columns.3
                        let (points, fallAt) = throwAim != nil ? ([aim.from, aim.from + aim.dir * 40], Int?.none)
                                                               : aimArc(from: aim.from, dir: aim.dir)
                        let clipFromOrigin = projectionOf(index) * viewMatrix
                        encoder.setRenderPipelineState(aimPipeline)
                        encoder.setDepthStencilState(aimDepthState)
                        // the line only as far as the arrow flies straight (the red dot); the fall beyond it, a sharp
                        // drop (the game's own: full speed to the bow's range, then down), looked like a broken line
                        // (device 2026-11-06). The light blue landing dot still comes from the whole arc below
                        let drawn = min(fallAt.map { $0 + 1 } ?? points.count, points.count)
                        for k in 1..<drawn where throwAim == nil {  // one strip a segment, faded along the line
                            var u = AimUniforms(clipFromOrigin: clipFromOrigin,
                                                from: SIMD4(points[k - 1], 1), to: SIMD4(points[k], 1), eye: eye,
                                                span: SIMD4(Float(k - 1) / Float(max(drawn - 1, 1)),
                                                            Float(k) / Float(max(drawn - 1, 1)), 0, 0))
                            encoder.setVertexBytes(&u, length: MemoryLayout<AimUniforms>.stride, index: 0)
                            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                        }
                        encoder.setRenderPipelineState(markPipeline)
                        var samples = resampled(points, every: 0.5, max: 240)
                        func mark(_ u: inout MarkUniforms) {
                            encoder.setVertexBytes(&u, length: MemoryLayout<MarkUniforms>.stride, index: 0)
                            samples.withUnsafeMutableBytes {
                                encoder.setVertexBytes($0.baseAddress!, length: max($0.count, 16), index: 1)
                            }
                            encoder.setVertexBytes(&params, length: MemoryLayout<ViewParams>.stride, index: 2)
                            encoder.setVertexTexture(eyeDepth ?? eyeTexture, index: 0)
                            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                        }
                        if let fallAt, fallAt < points.count {  // where it starts to fall, hidden behind the world
                            encoder.setDepthStencilState(aimDepthState)
                            var u = MarkUniforms(clipFromOrigin: clipFromOrigin, eyeFromOrigin: viewMatrix, eye: eye,
                                                 at: SIMD4(points[fallAt], 1), color: SIMD4(1.0, 0.2, 0.14, 0.8),
                                                 radius: 0.12, count: 0, mode: 0, pad: 0)
                            mark(&u)
                        }
                        // where it lands is looked for from 2.5 m out: grass right in front of the bow stopped the first
                        // samples and put the ring on the arrow (device 2026-11-03)
                        if samples.count > 6 { samples.removeFirst(5) }
                        if eyeDepth != nil, samples.count >= 2 {  // where it lands, over the world it lands on
                            encoder.setDepthStencilState(noDepthState)
                            var u = MarkUniforms(clipFromOrigin: clipFromOrigin, eyeFromOrigin: viewMatrix, eye: eye,
                                                 at: SIMD4(0, 0, 0, 1), color: SIMD4(0.4, 0.7, 1.0, 0.55),
                                                 radius: 0.16, count: UInt32(samples.count), mode: 1, pad: 0)
                            mark(&u)
                        }
                    }
                    if let originFromHud, let hud = viewHud {
                        let viewMatrix = (shownFromDevice * view.transform).inverse
                        var uniforms = ScreenUniforms(
                            clipFromScreen: projectionOf(index) * viewMatrix * originFromHud,
                            size: SIMD2<Float>(hudWidth, hudWidth * 9 / 16), hasHud: 1)
                        encoder.setRenderPipelineState(hudPipeline)
                        encoder.setDepthStencilState(noDepthState)
                        encoder.setVertexBytes(&uniforms, length: MemoryLayout<ScreenUniforms>.stride, index: 0)
                        encoder.setFragmentTexture(hud, index: 1)
                        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    }
                } else if havePicture, !settling, let originFromScreen, let eyeTexture = texture(index == 1 ? picture.eye.1 : picture.eye.0) {
                    let viewMatrix = (originFromDevice * view.transform).inverse
                    var uniforms = ScreenUniforms(
                        clipFromScreen: projectionOf(index) * viewMatrix * originFromScreen,
                        size: SIMD2<Float>(screenWidth, screenWidth * 9 / 16),
                        hasHud: picture.hud != nil ? 1 : 0)
                    encoder.setRenderPipelineState(pipeline)
                    encoder.setDepthStencilState(depthState)
                    encoder.setVertexBytes(&uniforms, length: MemoryLayout<ScreenUniforms>.stride, index: 0)
                    encoder.setFragmentBytes(&uniforms, length: MemoryLayout<ScreenUniforms>.stride, index: 0)
                    encoder.setFragmentTexture(eyeTexture, index: 0)
                    encoder.setFragmentTexture(texture(picture.hud) ?? eyeTexture, index: 1)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
        }
        encodeViews(drawable) { drawable.computeProjection(viewIndex: $0) }
        // the capture drawable: the left eye's picture again, drawn with the left eye's own projection so it fills the
        // capture's view edge to edge (a recording is seen flat, so its slightly narrower field does not show). Only a
        // second pass of the compositor over the eye images already made, not another game frame
        for capture in captures {
            capture.deviceAnchor = drawable.deviceAnchor
            encodeViews(capture) { _ in drawable.computeProjection(viewIndex: 0) }
            capture.encodePresent(commandBuffer: commandBuffer)
        }
        if captures.count != loggedCaptures {
            loggedCaptures = captures.count
            botw_vr_log("[vr] capture drawables: \(captures.count)" + (captures.first.map { c in
                c.colorTextures.first.map { ", \($0.width)x\($0.height), \(c.views.count) view(s)" } ?? "" } ?? ""))
        }
        drawable.encodePresent(commandBuffer: commandBuffer)
        let committed = CACurrentMediaTime()
        let deadline = LayerRenderer.Clock.Instant.epoch.duration(to: drawable.frameTiming.renderingDeadline).timeInterval
        let stats = gpuStats
        commandBuffer.addCompletedHandler { cb in  // GPU times share CACurrentMediaTime's (and ARKit's) clock
            stats.lock.lock()
            stats.n += 1
            if cb.gpuEndTime > deadline { stats.late += 1 }
            let wait = max(cb.gpuStartTime - committed, 0)
            stats.queueSum += wait
            stats.queueMax = max(stats.queueMax, wait)
            stats.runSum += cb.gpuEndTime - cb.gpuStartTime
            stats.lock.unlock()
        }
        commandBuffer.commit()
        frame.endSubmission()

        frames += 1
        if frames == 1 || frames % 900 == 0 {
            NSLog("[vr] compositor frame \(frames): picture \(havePicture ? "\(picture.stereo != 0 ? "stereo" : "mono") #\(picture.serial)" : "none"), "
                  + "\(drawable.views.count) views, head \(anchor != nil ? "tracked" : "not tracked")")
            stats.lock.lock()
            let n = max(stats.n, 1)
            let timing = String(format: "vr timing: late %d/%d, gpu queue avg %.1f ms max %.1f ms, gpu run avg %.1f ms, picture latency %.1f frames (%.0f ms), predict %@, shown 1x %d 2x %d 3x %d 4+x %d, correction avg %.1f max %.1f deg",
                  stats.late, stats.n, stats.queueSum / Double(n) * 1000, stats.queueMax * 1000, stats.runSum / Double(n) * 1000,
                  latencyFrames, latencyFrames * frameDuration * 1000, predict ? "on" : "off",
                  repeatCounts[0], repeatCounts[1], repeatCounts[2], repeatCounts[3],
                  correctionN > 0 ? correctionSum / Double(correctionN) : 0, correctionMax)
            NSLog("[vr] %@", timing)
            botw_vr_log(timing)  // also in runtime.log (a launch from the Home View has no console)
            // what may lower the system's frame rate (device 2026-11-06: the loop fell to 45 Hz after an explosion)
            let thermal = ["nominal", "fair", "serious", "critical"][min(ProcessInfo.processInfo.thermalState.rawValue, 3)]
            var quality = "?"
            if #available(visionOS 26.0, *) { quality = String(format: "%.2f", layerRenderer.renderQuality.rawValue) }
            botw_vr_log(String(format: "vr loop: %.1f Hz, thermal %@, render quality %@", 1 / max(frameDuration, 0.001), thermal, quality))
            // the view pass is the same work every frame, so its time is a proxy for the GPU's clock; with the game's
            // busy share in the [gpu] lines it gives the GPU's work per second
            botw_vr_log(String(format: "vr load: view pass %.2f ms each, %.0f ms of GPU a second", stats.runSum / Double(n) * 1000,
                               stats.runSum / max(Double(n) * frameDuration, 0.001) * 1000))
            if paceAuto || hudHold > 0 {  // the M2's (a fixed pace, the M5, logs as before)
                botw_vr_log(String(format: "vr pace: every %d refreshes or more (%@), %d switches; HUD missing in %d of %d pictures (%d gaps, longest %d), %d bridged (hold %d)",
                                   minimumShows, paceAuto ? "auto" : "fixed", paceSwitches, hudMissing, hudPictures, hudGaps,
                                   max(hudLongest, hudGap), hudBridged, hudHold))
            }
            hudPictures = 0; hudMissing = 0; hudGaps = 0; hudLongest = 0; hudBridged = 0
            stats.late = 0; stats.n = 0; stats.queueSum = 0; stats.queueMax = 0; stats.runSum = 0
            stats.lock.unlock()
            repeatCounts = [0, 0, 0, 0]
            correctionSum = 0; correctionMax = 0; correctionN = 0
        }
    }

    /// View mode: the head's predicted pose, each eye's place in the head and frustum, for the game's cameras.
    /// ARKitSession.run replaces what the session runs (seen on the device: running the controller tracking stopped the
    /// world tracking, and the other way round), so every run passes all of them
    private func runProviders() async {
        // a provider that has stopped cannot run again: fresh ones in their place
        if worldTracking.state == .stopped { worldTracking = WorldTrackingProvider() }
        if let tracked = accessoryTracking, tracked.state == .stopped {
            accessoryTracking = AccessoryTrackingProvider(accessories: trackedAccessories)
        }
        var providers: [any DataProvider] = [worldTracking]
        if let accessoryTracking { providers.append(accessoryTracking) }
        do { try await arSession.run(providers) } catch { NSLog("[vr] ARKit run: \(error)") }
    }

    /// Each hand's own spatial controller (not the merged pair, which is an ordinary gamepad) as a tracked accessory
    private func trackControllers() async {
        guard AccessoryTrackingProvider.isSupported else { return }
        let spatial = GCController.controllers().filter {
            $0.extendedGamepad == nil && $0.productCategory == GCProductCategorySpatialController
        }
        let ids = spatial.map { ObjectIdentifier($0) }
        if ids == trackedControllers { return }
        var accessories: [Accessory] = []
        for controller in spatial {
            do { accessories.append(try await Accessory(device: controller)) } catch {
                NSLog("[vr] controller \(controller.vendorName ?? "?") not trackable: \(error)")
            }
        }
        do {
            if #available(visionOS 27.0, *), let accessoryTracking {
                try await accessoryTracking.updateAccessories(accessories)
            } else {
                accessoryTracking = AccessoryTrackingProvider(accessories: accessories)
                worldTracking = WorldTrackingProvider()
                await runProviders()
            }
            trackedAccessories = accessories
            trackedControllers = ids
            NSLog("[vr] tracking \(accessories.count) controller(s): \(accessories.map { "\($0.name) \($0.inherentChirality)" })")
        } catch { NSLog("[vr] controller tracking: \(error)") }
    }

    /// The controllers' grips into the pose, predicted for the presentation time (slot 0 left hand, 1 right)
    /// The bow's aim, from the controllers as vr_bow.cpp aims the arrow: from the bow hand along the line from the
    /// drawing hand through it once they are 15 cm apart, else where the bow hand points; nil unless the bow is drawn
    /// The arrow's path from the launch point along the aim, as the game flies it (BetterVR bow.cpp
    /// BuildPreviewTrajectory): its speed eases towards the bow's steady speed; past the bow's range it falls, easing
    /// to the falling speed with the gravity added every frame. 48 frames of 1/30 s; gravity is -y in the room as in
    /// the game's world (the view only turns about y, 1:1 in metres). Without the bow's numbers a straight 40 m line
    private func aimArc(from: SIMD3<Float>, dir: SIMD3<Float>) -> ([SIMD3<Float>], Int?) {
        var f = [Float](repeating: 0, count: 8)
        guard botw_vr_bow_flight(&f) != 0, f[0] > 0.01 else { return ([from, from + dir * 40], nil) }
        let (accel, steady, fallAccel, fallSteady, gravity, range) = (f[1], f[2], f[3], f[4], f[5], f[6])
        // the arrow steps every game frame by its speed times the frame's length in 1/30 s (BetterVR's vfrScale), but the
        // gravity is added whole each frame: at 45 fps it falls sooner than a 30 fps sum says, and lands 3-22 m nearer the
        // farther and higher the shot (device 2026-11-09, "lands short of the line")
        let step = min(max(f[7], 0.25), 1.5)
        func integrate(_ speed: Float, _ accel: Float, _ target: Float) -> Float {
            let next = speed + accel * step
            return accel < -1e-7 ? max(next, target) : accel > 1e-7 ? min(next, target) : speed
        }
        var points = [from]
        var pos = from, speed = integrate(f[0], accel, steady)
        pos += dir * 0.01 * step  // the first frame: a short step out of the bow
        points.append(pos)
        var velocity = dir * speed, falling = false, a = accel, target = steady, bias = SIMD3<Float>(0, 0, 0)
        var fallAt: Int?
        for _ in 2...180 {  // long enough to come down from a high shot at the frame rate above (48 ended in the air)
            if !falling {
                let fall = range <= 10 || simd_distance(pos, from) >= range
                speed = integrate(speed, a, target)
                velocity = dir * speed
                pos += velocity * step
                points.append(pos)
                if fall {
                    falling = true
                    fallAt = points.count - 1
                    a = fallAccel
                    // the falling speed: by the range (20 m the base), at most the steady speed; kept when it has none
                    target = min(max(fallSteady + (range / 20 - 1), 0), steady)
                    if abs(fallAccel) < 1e-6 { target = speed != 0 ? speed : abs(gravity) }
                    bias = SIMD3(0, gravity, 0)
                }
                continue
            }
            speed = integrate(speed, a, target)
            let along = simd_length(velocity) > 1e-3 ? simd_normalize(velocity) * speed : velocity * speed
            velocity = along + bias
            pos += velocity * step
            points.append(pos)
            if pos.y < from.y - 30 { break }  // far below: the ground has long stopped it
        }
        return (points, fallAt)
    }

    /// The arc again at even steps along it (for mark_vertex's search), packed x, y, z
    private func resampled(_ points: [SIMD3<Float>], every step: Float, max: Int) -> [Float] {
        var out: [Float] = []
        var carry: Float = 0
        for k in 1..<points.count {
            let a = points[k - 1], b = points[k], len = simd_distance(a, b)
            var t = carry
            while t < len, out.count / 3 < max {
                let p = a + (b - a) * (t / len)
                out += [p.x, p.y, p.z]
                t += step
            }
            carry = t - len
        }
        return out
    }

    /// Which hand a controller is in. The Sense controllers are made for one hand each, so their own side comes first;
    /// the held side the system guesses could swap for a while and cross Link's arms (device 2026-11-03)
    private func chirality(_ a: AccessoryAnchor) -> Accessory.Chirality {
        let own = a.accessory.inherentChirality
        return own != .unspecified ? own : (a.heldChirality ?? .unspecified)
    }

    /// A throw's line in the room: from 0.35 m ahead of and 0.15 m above the eyes along where the head looks, as
    /// vr_aim.cpp launches it
    private func throwRay(_ originFromDevice: simd_float4x4) -> (from: SIMD3<Float>, dir: SIMD3<Float>)? {
        let c = originFromDevice.columns
        let dir = -SIMD3(c.2.x, c.2.y, c.2.z)
        guard simd_length(dir) > 1e-4 else { return nil }
        let d = simd_normalize(dir)
        return (SIMD3(c.3.x, c.3.y, c.3.z) + d * 0.35 + SIMD3(0, 0.15, 0), d)
    }

    private func aimRay(at time: TimeInterval) -> (from: SIMD3<Float>, dir: SIMD3<Float>)? {
        guard botw_vr_bow_aiming() != 0, let accessoryTracking else { return nil }
        var grips: [Int: simd_float4x4] = [:]
        for latest in accessoryTracking.latestAnchors where latest.trackingState != .untracked && latest.trackingState != .orientationTracked {
            let a = accessoryTracking.predictAnchor(for: latest, at: time) ?? latest
            let hand = chirality(a)
            guard hand != .unspecified else { continue }
            if hand == .left {
                grips[0] = a.coordinateSpace(for: .grip, correction: .none).ancestorFromSpaceTransformFloat().matrix
                grips[2] = a.coordinateSpace(for: .aim, correction: .none).ancestorFromSpaceTransformFloat().matrix
            }
        }
        guard let left = grips[0], let aim = grips[2] else { return nil }
        let l = SIMD3(left.columns.3.x, left.columns.3.y, left.columns.3.z)
        // where the left controller points (its aim space, not the grip's -z, which runs along the handle), as the
        // arrow's aim (vr_bow.cpp launch): the right hand does not steer it
        var dir = -SIMD3(aim.columns.2.x, aim.columns.2.y, aim.columns.2.z)
        // the way the bow shoots (its -x, as the shot uses, vr_bow.cpp), turned with the controller as it is now
        var local = [Float](repeating: 0, count: 3)
        if botw_vr_bow_dir_in_grip(&local) != 0 {
            let v = left * SIMD4(local[0], local[1], local[2], 0)
            dir = SIMD3(v.x, v.y, v.z)
        }
        guard simd_length(dir) > 1e-4 else { return nil }
        dir = simd_normalize(dir)
        // where the arrow sits (the bow's launch point, vr_bow.cpp launch), carried with the controller as it is now
        var origin = [Float](repeating: 0, count: 3)
        if botw_vr_bow_origin_in_grip(&origin) != 0 {
            let p = left * SIMD4(origin[0], origin[1], origin[2], 1)
            return (SIMD3(p.x, p.y, p.z), dir)
        }
        return (l + dir * 0.08, dir)
    }

    private func addHands(_ pose: inout BotWVRPose, at time: TimeInterval) {
        guard let accessoryTracking else { return }
        var grips = [simd_float4x4](repeating: matrix_identity_float4x4, count: 2)
        var motion = [Float](repeating: 0, count: 12)  // velocity[2][3], then angular velocity[2][3]
        var tracked: [UInt32] = [0, 0]
        var aims = [Float](repeating: 0, count: 6)
        for latest in accessoryTracking.latestAnchors {
            let a = accessoryTracking.predictAnchor(for: latest, at: time) ?? latest
            let hand = chirality(a)
            guard hand != .unspecified, a.trackingState != .untracked else { continue }
            let i = hand == .left ? 0 : 1
            grips[i] = a.coordinateSpace(for: .grip, correction: .none).ancestorFromSpaceTransformFloat().matrix
            for k in 0..<3 {
                motion[i * 3 + k] = a.velocity[k]
                motion[6 + i * 3 + k] = a.angularVelocity[k]
            }
            tracked[i] = a.trackingState == .orientationTracked ? 1 : 2
            let aim = a.coordinateSpace(for: .aim, correction: .none).ancestorFromSpaceTransformFloat().matrix
            let local = grips[i].inverse * SIMD4(-aim.columns.2.x, -aim.columns.2.y, -aim.columns.2.z, 0)
            for k in 0..<3 { aims[i * 3 + k] = local[k] }
        }
        withUnsafeMutableBytes(of: &pose.origin_from_grip) { dst in grips.withUnsafeBytes { dst.copyMemory(from: $0) } }
        withUnsafeMutableBytes(of: &pose.velocity) { dst in motion[0..<6].withUnsafeBytes { dst.copyMemory(from: $0) } }
        withUnsafeMutableBytes(of: &pose.angular_velocity) { dst in motion[6..<12].withUnsafeBytes { dst.copyMemory(from: $0) } }
        pose.hand_tracked = (tracked[0], tracked[1])
        withUnsafeMutableBytes(of: &pose.grip_aim) { dst in aims.withUnsafeBytes { dst.copyMemory(from: $0) } }
    }

    private func publishPose(_ anchor: DeviceAnchor, _ drawable: LayerRenderer.Drawable, presentation: TimeInterval) {
        poseID &+= 1
        var pose = BotWVRPose()
        pose.id = poseID
        // the head (and the controllers) where they will be when the picture shows, 2 to 8 headset frames on
        let shownAt = presentation + (predict ? min(max(latencyFrames, 2), 8) * frameDuration : 0)
        let rendered = predict ? worldTracking.queryDeviceAnchor(atTimestamp: shownAt) ?? anchor : anchor
        var head = rendered.originFromAnchorTransform
        withUnsafeMutableBytes(of: &pose.origin_from_device) { dst in withUnsafeBytes(of: &head) { dst.copyMemory(from: $0) } }
        var eyes = [simd_float4x4](repeating: matrix_identity_float4x4, count: 2)
        var tangents = [Float](repeating: 0, count: 8)
        for i in 0..<2 {
            let v = min(i, drawable.views.count - 1)
            eyes[i] = drawable.views[v].transform
            // from the projection: clip x = p00 x + p20 z over -z, so the edges (ndc -1, 1) are at (p20 -+ 1) / p00
            let p = drawable.computeProjection(viewIndex: v)
            // widened by the overscan: the game draws a margin round the view for the warp in renderFrame
            tangents[i * 4 + 0] = (p.columns.2.x - 1) / p.columns.0.x * overscan * crop[i].x
            tangents[i * 4 + 1] = (p.columns.2.x + 1) / p.columns.0.x * overscan * crop[i].y
            tangents[i * 4 + 2] = (p.columns.2.y + 1) / p.columns.1.y * overscan * crop[i].z
            tangents[i * 4 + 3] = (p.columns.2.y - 1) / p.columns.1.y * overscan * crop[i].w
        }
        withUnsafeMutableBytes(of: &pose.device_from_view) { dst in eyes.withUnsafeBytes { dst.copyMemory(from: $0) } }
        withUnsafeMutableBytes(of: &pose.tangents) { dst in tangents.withUnsafeBytes { dst.copyMemory(from: $0) } }
        addHands(&pose, at: shownAt)
        botw_vr_set_pose(&pose)
        anchors[UInt8(truncatingIfNeeded: poseID)] = rendered  // handed back with its picture: the pose it was drawn for
        publishedAt[UInt8(truncatingIfNeeded: poseID)] = frames
        if poseID == 1 {
            NSLog("[vr] first pose: tangents left eye %.2f %.2f %.2f %.2f", tangents[0], tangents[1], tangents[2], tangents[3])
            // in runtime.log too, without the overscan and crop: the headset's own field, for the edge measurement
            let k = overscan
            botw_vr_log(String(format: "[vr] view tangents (left, right, top, bottom) left eye %.3f %.3f %.3f %.3f, right eye %.3f %.3f %.3f %.3f",
                               tangents[0] / k / crop[0].x, tangents[1] / k / crop[0].y, tangents[2] / k / crop[0].z, tangents[3] / k / crop[0].w,
                               tangents[4] / k / crop[1].x, tangents[5] / k / crop[1].y, tangents[6] / k / crop[1].z, tangents[7] / k / crop[1].w))
        }
    }

    /// A panel in front of the head at the moment the space opened: same height, turned to face it (yaw only).
    private func placeScreen(_ originFromDevice: simd_float4x4, distance screenDistance: Float) -> simd_float4x4 {
        let position = SIMD3<Float>(originFromDevice.columns.3.x, originFromDevice.columns.3.y, originFromDevice.columns.3.z)
        var forward = -SIMD3<Float>(originFromDevice.columns.2.x, 0, originFromDevice.columns.2.z)
        if simd_length(forward) < 1e-3 { forward = SIMD3<Float>(0, 0, -1) }
        forward = simd_normalize(forward)
        let right = simd_normalize(simd_cross(forward, SIMD3<Float>(0, 1, 0)))
        let center = position + forward * screenDistance
        // the screen's +z points back at the viewer
        return simd_float4x4(SIMD4<Float>(right, 0), SIMD4<Float>(0, 1, 0, 0), SIMD4<Float>(-forward, 0),
                             SIMD4<Float>(center, 1))
    }

    /// The loading panel's picture (premultiplied, sRGB-encoded BGRA as the HUD's): the name, what is happening, a
    /// progress bar and a tip, redrawn four times a second. The progress is an estimate by stage and time (the game
    /// reports none): starting up until it draws, then the title and the save loading until its view shows
    /// The loading panel is a spinner, nothing more (user 2026-11-03: the card with a title, a progress and tips looked
    /// out of place next to the game, and its numbers went past 100 %): twelve dots round a ring, the bright one going
    /// round once a second, Sheikah blue on nothing
    /// When the start takes long, under it, one short line of what is happening and why (user 2026-11-11: a minute or two
    /// of the spinner alone looked like a hang): the shaders compiled so far count up; no percentage, the game reports
    /// no total
    private func updateLoadingPanel() {
        guard Date().timeIntervalSince(loadingDrawnAt) > 1.0 / 30 else { return }
        loadingDrawnAt = Date()
        let w = 1024, h = 512, spin = 256
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return }
        ctx.clear(CGRect(x: 0, y: 0, width: w, height: h))
        let turn = Date().timeIntervalSince(loadingStart).truncatingRemainder(dividingBy: 1)
        // the spinner in the upper half (CoreGraphics' y goes up)
        let dots = 12, cx = CGFloat(w) / 2, cy = CGFloat(h) - CGFloat(spin) / 2, r = CGFloat(spin) * 0.36
        for k in 0..<dots {
            let a = CGFloat(k) / CGFloat(dots) * 2 * .pi
            let lag = (CGFloat(turn) * CGFloat(dots) - CGFloat(k) + CGFloat(dots)).truncatingRemainder(dividingBy: CGFloat(dots))
            let alpha = max(0.15, 1 - lag / CGFloat(dots) * 1.2)
            let d = CGFloat(spin) * 0.075 * (0.7 + 0.3 * alpha)
            ctx.setFillColor(CGColor(srgbRed: 0.3, green: 0.85, blue: 0.95, alpha: alpha))
            ctx.fillEllipse(in: CGRect(x: cx + r * sin(a) - d / 2, y: cy + r * cos(a) - d / 2, width: d, height: d))
        }
        // only a long start (more than 5 s before the title menu): the shaders recorded in the cache being compiled again,
        // which takes a minute or more the first time after an install or an update (the system's Metal cache does not
        // match the new app); with no cache at all, or a warm one, the title comes in seconds and only the spinner shows
        // (device 2026-11-12: no cache at all reached the title in 3 s, the public build's first start in 76 s)
        if phase == 1 && Date().timeIntervalSince(phaseStart) > 5 {
            let compiled = Int(botw_vr_compiles())
            drawCentered(compiled > 0 ? tr("vr.loading.shaders", compiled) : tr("vr.loading.boot"), in: ctx,
                         y: CGFloat(h) - CGFloat(spin) - 70, size: 44, alpha: 0.9, width: w)
            drawCentered(tr("vr.loading.first"), in: ctx, y: CGFloat(h) - CGFloat(spin) - 140, size: 32, alpha: 0.6, width: w)
        }
        if loadingTexture == nil {
            let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: w, height: h, mipmapped: false)
            desc.usage = .shaderRead
            loadingTexture = device.makeTexture(descriptor: desc)
        }
        if let data = ctx.data {
            loadingTexture?.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: data, bytesPerRow: w * 4)
        }
    }

    // centred on the panel, never cut off (device 2026-11-18: the long first-launch line ran past both edges): a line
    // too wide shrinks to three quarters of its size at most, then wraps into two lines split near the middle, at a
    // space (or a Chinese comma), the second below the first
    private func drawCentered(_ text: String, in ctx: CGContext, y: CGFloat, size: CGFloat, alpha: CGFloat, width: Int) {
        let room = CGFloat(width) - 64
        func line(_ t: String, _ pt: CGFloat) -> (CTLine, CGFloat) {
            let font = CTFontCreateUIFontForLanguage(.system, pt, nil) ?? CTFontCreateWithName("Helvetica" as CFString, pt, nil)
            let attrs: [NSAttributedString.Key: Any] = [
                NSAttributedString.Key(kCTFontAttributeName as String): font,
                NSAttributedString.Key(kCTForegroundColorAttributeName as String): CGColor(srgbRed: 1, green: 1, blue: 1, alpha: alpha),
            ]
            let l = CTLineCreateWithAttributedString(NSAttributedString(string: t, attributes: attrs))
            return (l, CGFloat(CTLineGetTypographicBounds(l, nil, nil, nil)))
        }
        func draw(_ l: CTLine, _ lw: CGFloat, _ at: CGFloat) {
            ctx.textPosition = CGPoint(x: (CGFloat(width) - lw) / 2, y: at)
            CTLineDraw(l, ctx)
        }
        var (l, lw) = line(text, size)
        if lw > room {
            let pt = max(size * 0.75, size * room / lw)
            (l, lw) = line(text, pt)
            if lw > room {
                // two lines, split at the break nearest the middle
                let chars = Array(text)
                let mid = chars.count / 2
                let breaks = chars.indices.filter { chars[$0] == " " || chars[$0] == "，" }
                let cut = breaks.min { abs($0 - mid) < abs($1 - mid) } ?? mid
                let first = String(chars[..<(chars[cut] == "，" ? cut + 1 : cut)]).trimmingCharacters(in: .whitespaces)
                let second = String(chars[min(cut + 1, chars.count)...]).trimmingCharacters(in: .whitespaces)
                let (l1, w1) = line(first, pt), (l2, w2) = line(second, pt)
                draw(l1, min(w1, room), y)
                draw(l2, min(w2, room), y - pt * 1.3)
                return
            }
        }
        draw(l, lw, y)
    }

    /// BOTW_VR_PACE=auto: a stereo picture left the screen after `heldFrames` shows; switch the pace when the window says so
    private func tallyPace(old: BotWVRFrame, new: BotWVRFrame) {
        guard paceAuto else { return }
        let slow = paceSlowLoop ? frameDuration > 0.013 : frameDuration > 0.015
        if slow != paceSlowLoop {  // the same picture rate in the new loop's frames, as near as it goes
            paceSlowLoop = slow
            minimumShows = slow ? (minimumShows + 1) / 2 : min(minimumShows * 2, 4)
            paceWindow = 0; paceMissed = 0; paceFits = 0
            paceSwitchedAt = Date()
            botw_vr_set_pace(Int32(minimumShows))
            botw_vr_log(String(format: "vr pace: loop at %.0f Hz, every %d refreshes", 1 / max(frameDuration, 0.001), minimumShows))
        }
        // only pictures of play count (Fable's review 2026-11-23 of the same log: the inventory and pause menu are stereo
        // with the HUD on, the game nearly idle at 10.7 ms a frame, and twice that stepped the pace down to 1 in the menu;
        // back in the world, at 33 fps, pictures mixed 1 and 2 shows for up to 22 s): menus, loading and maps restart the
        // window and the step-down count
        guard old.stereo != 0, new.stereo != 0, botw_vr_in_play() != 0 else {
            paceWindow = 0; paceMissed = 0; paceFits = 0
            return
        }
        paceWindow += 1
        if heldFrames > minimumShows { paceMissed += 1 }
        // within 30 s of a step down, a step back up is judged every 45 pictures (otherwise 90: the 3 -> 4 step sits near
        // its line and a short window would take bunched late pictures for a slower game)
        let probation = Date().timeIntervalSince(paceDownAt) < 30
        guard paceWindow >= 90 || (probation && paceWindow >= 45) else { return }
        if paceWindow < 90 {  // a probation check: a step back up only, the window goes on to 90
            let lowest = paceSlowLoop ? 1 : 2, highest = paceSlowLoop ? 2 : 4
            if minimumShows >= lowest, minimumShows < highest, paceMissed * 5 >= paceWindow {
                let missed = paceMissed, window = paceWindow
                paceWindow = 0; paceMissed = 0
                paceUp(missed: missed, window: window)
            }
            return
        }
        let missed = paceMissed, window = paceWindow
        paceWindow = 0; paceMissed = 0
        let since = Date().timeIntervalSince(paceSwitchedAt)
        let busy = Double(botw_vr_game_busy_ms())
        let fasterBudget = Double(minimumShows - 1) * frameDuration * 1000 * 0.85
        let lowest = paceSlowLoop ? 1 : 2, highest = paceSlowLoop ? 2 : 4
        // one line for every step up (20%; the lowest level's 40% kept the 1/2 mix at 45 Hz for 22 s)
        if minimumShows < highest, missed * 5 >= window {
            paceUp(missed: missed, window: window)
        } else if minimumShows > lowest, missed * 20 <= window, busy > 0, busy < fasterBudget {
            paceFits += 1
            if paceFits >= 2, since >= paceDwell {
                paceSwitch(to: minimumShows - 1, String(format: "fits %.1f ms", fasterBudget), busy: busy)
            }
        } else {
            paceFits = 0
        }
    }

    private func paceUp(missed: Int, window: Int) {
        paceSwitch(to: minimumShows + 1, "\(missed) of \(window) pictures late", busy: Double(botw_vr_game_busy_ms()))
    }

    private func paceSwitch(to n: Int, _ why: String, busy: Double) {
        if n > minimumShows, Date().timeIntervalSince(paceDownAt) < 30 { paceDwell = min(paceDwell * 2, 160) }
        if n < minimumShows { paceDownAt = Date() }
        minimumShows = n
        paceSwitches += 1
        paceSwitchedAt = Date()
        paceFits = 0
        botw_vr_set_pace(Int32(n))
        botw_vr_log(String(format: "vr pace: every %d refreshes (%@; game frame %.1f ms, in play %d, next wait %.0f s)",
                           n, why, busy, botw_vr_in_play(), paceDwell))
    }

    private func texture(_ pointer: UnsafeMutableRawPointer?) -> MTLTexture? {
        guard let pointer else { return nil }
        return Unmanaged<MTLTexture>.fromOpaque(pointer).takeUnretainedValue()
    }

    private struct ViewParams {  // view_fragment's
        var sharpness: Float
        var constDepth: Float
        var useDepth: Float
        var projA: Float
        var projB: Float
        var near: Float
        var far: Float
        var zScale: Float
        var zOffset: Float
        var maxDistance: Float
        var fade: Float
        var bicubic: Float
        var fov = SIMD4<Float>(repeating: 1)  // BOTW_VR_FOV: the part of the view that shows the picture, per edge
        var edgeRings: Float = 0        // BOTW_VR_EDGE_RINGS
        var edgeFade: Float = 0.03      // BOTW_VR_EDGE_FADE
        var edgeFill: Float = 0.5       // BOTW_VR_EDGE_FILL (was pad1)
        var pad2: Float = 0
        var viewEdges: SIMD4<Float>     // tangents of the view: left, right, top, bottom
        var pictureEdges: SIMD4<Float>  // and of the picture (wider by the overscan)
        var turn0: SIMD4<Float>         // the eye the picture was drawn for <- the eye now (rotation columns)
        var turn1: SIMD4<Float>
        var turn2: SIMD4<Float>
    }
    private struct MarkUniforms {  // mark_vertex's
        var clipFromOrigin: simd_float4x4
        var eyeFromOrigin: simd_float4x4
        var eye: SIMD4<Float>
        var at: SIMD4<Float>
        var color: SIMD4<Float>
        var radius: Float
        var count: UInt32
        var mode: UInt32
        var pad: UInt32
    }
    private struct AimUniforms {  // aim_vertex's
        var clipFromOrigin: simd_float4x4
        var from: SIMD4<Float>
        var to: SIMD4<Float>
        var eye: SIMD4<Float>
        var span: SIMD4<Float>  // where the segment starts and ends along the whole line (0 to 1)
    }
    private struct ScreenUniforms {
        var clipFromScreen: simd_float4x4
        var size: SIMD2<Float>
        var hasHud: UInt32
    }

    // The pictures hold sRGB-encoded colours in a BGRA8Unorm texture, the HUD premultiplied in that encoding: blend
    // as the game's screen would, then decode for the sRGB drawable.
    private static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;
    struct Uniforms { float4x4 clipFromScreen; float2 size; uint hasHud; };
    struct VertexOut { float4 position [[position]]; float2 uv; };
    vertex VertexOut screen_vertex(ushort vid [[vertex_id]], constant Uniforms& u [[buffer(0)]]) {
        float2 corner = float2(vid & 1, vid >> 1);
        VertexOut out;
        out.position = u.clipFromScreen * float4((corner - 0.5) * u.size, 0.0, 1.0);
        out.uv = float2(corner.x, 1.0 - corner.y);
        return out;
    }
    static float3 decode(float3 c) {
        return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
    }
    struct ViewOut { float4 position [[position]]; float2 uv; };
    vertex ViewOut view_vertex(ushort vid [[vertex_id]]) {
        float2 pos = vid == 0 ? float2(-1.0, -3.0) : vid == 1 ? float2(-1.0, 1.0) : float2(3.0, 1.0);
        ViewOut out;
        out.position = float4(pos, 0.0, 1.0);
        out.uv = float2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);
        return out;
    }
    // AMD FidelityFX CAS's idea (contrast-adaptive sharpening): the cross of neighbours pulls the centre away from
    // their average, less where the local contrast is already high, so edges sharpen without ringing
    struct ViewParams {
        float sharpness, constDepth, useDepth, projA, projB, near, far, zScale, zOffset, maxDistance, fade, bicubic;
        float4 fov;
        float edgeRings, edgeFade, edgeFill, pad2;
        float4 viewEdges, pictureEdges, turn0, turn1, turn2;
    };
    struct ViewPixel { float4 color [[color(0)]]; float depth [[depth(any)]]; };
    // The game's depth (an OpenGL window depth, (ndc + 1) / 2, of its device matrix: ndc = zScale (z_gl + zOffset))
    // back to the distance ahead, then into the compositor's reverse-Z projection
    // the game's depth as the distance ahead of the eye the picture was drawn for
    static float game_distance(float d, constant ViewParams& p) {
        const float ndc = (2.0 * d - 1.0) / max(p.zScale, 1e-6) - p.zOffset;
        const float denom = (p.far + p.near) - ndc * (p.far - p.near);
        return denom > 0 ? min(2.0 * p.far * p.near / denom, p.maxDistance) : p.maxDistance;
    }
    static float3 eye_tap(texture2d<float> tex, sampler s, float2 uv) { return saturate(tex.sample(s, uv).rgb); }
    // Catmull-Rom in five bilinear taps (the four corner taps of the full 4x4 weigh almost nothing)
    static float3 catmull_rom(texture2d<float> tex, sampler s, float2 uv) {
        const float2 size = float2(tex.get_width(), tex.get_height());
        const float2 at = uv * size;
        const float2 p1 = floor(at - 0.5) + 0.5;
        const float2 f = at - p1;
        const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
        const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
        const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
        const float2 w3 = f * f * (-0.5 + 0.5 * f);
        const float2 w12 = w1 + w2;
        const float2 p12 = (p1 + w2 / w12) / size;
        const float2 p0 = (p1 - 1.0) / size;
        const float2 p3 = (p1 + 2.0) / size;
        float3 sum = eye_tap(tex, s, float2(p12.x, p0.y)) * w12.x * w0.y;
        sum += eye_tap(tex, s, float2(p0.x, p12.y)) * w0.x * w12.y;
        sum += eye_tap(tex, s, p12) * w12.x * w12.y;
        sum += eye_tap(tex, s, float2(p3.x, p12.y)) * w3.x * w12.y;
        sum += eye_tap(tex, s, float2(p12.x, p3.y)) * w12.x * w3.y;
        const float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
        return sum / weight;
    }
    fragment ViewPixel view_fragment(ViewOut in [[stage_in]], texture2d<float> eye [[texture(0)]],
                                     texture2d<float> depth [[texture(1)]], constant ViewParams& p [[buffer(0)]]) {
        constexpr sampler s(filter::linear, address::clamp_to_edge);
        constexpr sampler nearest(filter::nearest, address::clamp_to_edge);
        ViewPixel out;
        // this pixel's ray in the eye now (tangents), turned into the eye the picture was drawn for, and where it
        // lands in the picture (identity turn and equal edges: the pixel's own place)
        const float3 ray_now = float3(mix(p.viewEdges.x, p.viewEdges.y, in.uv.x), mix(p.viewEdges.z, p.viewEdges.w, in.uv.y), -1.0);
        // beyond the cropped field (BOTW_VR_FOV), out of the lenses' sight: the picture's edge carried on, dimmed to
        // edgeFill (one bilinear tap, no depth), or black with edgeFill 0; before the full path below. About half the
        // view at the default crop, in a pass that runs twice a game frame (90 Hz)
        if (p.edgeRings == 0 && (ray_now.x < p.viewEdges.x * p.fov.x || ray_now.x > p.viewEdges.y * p.fov.y ||
                                 ray_now.y > p.viewEdges.z * p.fov.z || ray_now.y < p.viewEdges.w * p.fov.w)) {
            float3 fill = float3(0.0);
            if (p.edgeFill > 0) {
                const float2 lo = float2(p.viewEdges.x * p.fov.x, p.viewEdges.w * p.fov.w);
                const float2 hi = float2(p.viewEdges.y * p.fov.y, p.viewEdges.z * p.fov.z);
                const float3 rc = float3(clamp(ray_now.xy, lo, hi), -1.0);
                const float3 r = float3x3(p.turn0.xyz, p.turn1.xyz, p.turn2.xyz) * rc;
                const float2 tp = r.xy / max(-r.z, 1e-4);
                const float2 uv = float2((tp.x - p.pictureEdges.x) / (p.pictureEdges.y - p.pictureEdges.x),
                                         (p.pictureEdges.z - tp.y) / (p.pictureEdges.z - p.pictureEdges.w));
                fill = decode(saturate(eye.sample(s, saturate(uv)).rgb)) * p.edgeFill * p.fade;
            }
            out.color = float4(fill, 1.0);
            // a real depth, far away (50 m), not the cleared 0: the system takes depth 0 for "nothing here", and the
            // recordings and screenshots, which reproject the view by its depth, showed this part black, a band down
            // the right side once the crop came in (device 2026-11-15: gone with BOTW_VR_FOV=1). The headset looked right
            out.depth = p.constDepth > 0 ? p.constDepth : -p.projA + p.projB / 50.0;
            return out;
        }
        const float3x3 turn = float3x3(p.turn0.xyz, p.turn1.xyz, p.turn2.xyz);
        const float3 ray = turn * ray_now;
        const float2 tan_pic = ray.xy / max(-ray.z, 1e-4);
        const float2 uv = float2((tan_pic.x - p.pictureEdges.x) / (p.pictureEdges.y - p.pictureEdges.x),
                                 (p.pictureEdges.z - tan_pic.y) / (p.pictureEdges.z - p.pictureEdges.w));
        const bool inside = ray.z < 0 && all(uv >= 0.0) && all(uv <= 1.0);
        if (p.useDepth > 0) {
            // the point at the game's distance along the turned ray, back in the eye now: its distance ahead
            const float ahead = game_distance(depth.sample(nearest, uv).r, p);
            const float3 point_pic = float3(tan_pic * ahead, -ahead);
            const float z_now = max(-(transpose(turn) * point_pic).z, 1e-3);
            out.depth = -p.projA + p.projB / z_now;
        } else {
            out.depth = p.constDepth;
        }
        const float sharpness = p.sharpness;
        float3 c = p.bicubic > 0 ? saturate(catmull_rom(eye, s, uv)) : saturate(eye.sample(s, uv).rgb);
        if (sharpness > 0) {
            const float2 t = 1.0 / float2(eye.get_width(), eye.get_height());
            const float3 n = saturate(eye.sample(s, uv + float2(0, -t.y)).rgb);
            const float3 so = saturate(eye.sample(s, uv + float2(0, t.y)).rgb);
            const float3 e = saturate(eye.sample(s, uv + float2(t.x, 0)).rgb);
            const float3 w = saturate(eye.sample(s, uv + float2(-t.x, 0)).rgb);
            const float3 mn = min(c, min(min(n, so), min(e, w)));
            const float3 mx = max(c, max(max(n, so), max(e, w)));
            const float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
            const float3 k = -amp * mix(0.125, 0.2, sharpness);
            c = saturate((c + (n + so + e + w) * k) / (1.0 + 4.0 * k));
        }
        // BOTW_VR_FOV below 1: black beyond that part of the view, fixed to the display (the overscan's margin is still
        // round it for the warp), the last 3% fading out instead of a hard edge
        float edge = 1.0;
        if (any(p.fov < 0.999)) {
            const float2 lo = float2(p.viewEdges.x * p.fov.x, p.viewEdges.w * p.fov.w);
            const float2 hi = float2(p.viewEdges.y * p.fov.y, p.viewEdges.z * p.fov.z);
            const float2 d = min((ray_now.xy - lo) / (hi - lo), (hi - ray_now.xy) / (hi - lo));
            // only the cropped edges fade (an edge at 1 is the display's own)
            const float2 dl = float2(p.fov.x < 0.999 ? d.x : 1.0, p.fov.w < 0.999 ? d.y : 1.0);
            const float2 dh = float2(p.fov.y < 0.999 ? d.x : 1.0, p.fov.z < 0.999 ? d.y : 1.0);
            const float2 dd = float2(ray_now.x < (lo.x + hi.x) * 0.5 ? dl.x : dh.x, ray_now.y < (lo.y + hi.y) * 0.5 ? dl.y : dh.y);
            edge = mix(p.edgeFill, 1.0, smoothstep(0.0, p.edgeFade, min(dd.x, dd.y)));  // down to the fill beyond
        }
        out.color = float4(decode(c) * (inside ? p.fade * edge : 0.0), 1.0);  // beyond the picture's margin: black
        if (p.edgeRings > 0) {
            // how far out this pixel is towards each edge it faces (0 on the eye's axis, 1 on the view's edge)
            const float fx = ray_now.x < 0 ? ray_now.x / p.viewEdges.x : ray_now.x / p.viewEdges.y;
            const float fy = ray_now.y > 0 ? ray_now.y / p.viewEdges.z : ray_now.y / p.viewEdges.w;
            const float levels[5] = {0.50, 0.60, 0.70, 0.80, 0.90};  // device 2026-11-10: only the bottom's 80 % showed
            const float3 colours[5] = {float3(1, 0.1, 0.1), float3(1, 0.55, 0), float3(1, 0.95, 0), float3(0.1, 1, 0.2), float3(0.2, 0.5, 1)};
            const float w = 0.012;  // about 1 % of the field each side: thin rings vanished in the foveated edge (device 2026-11-10)
            for (int i = 0; i < 5; i++) {
                const float l = levels[i];
                if ((abs(fx - l) < w && fy <= l + w) || (abs(fy - l) < w && fx <= l + w)) out.color = float4(colours[i], 1.0);
            }
        }
        return out;
    }
    struct AimUniforms { float4x4 clipFromOrigin; float4 from; float4 to; float4 eye; float4 span; };
    struct AimOut { float4 position [[position]]; float t; float across; };
    // a thin strip facing the eye along the aim, a little wider farther away (about the same width on screen)
    vertex AimOut aim_vertex(ushort vid [[vertex_id]], constant AimUniforms& u [[buffer(0)]]) {
        const float3 a = u.from.xyz, b = u.to.xyz;
        const float3 p = vid < 2 ? a : b;
        const float3 side = normalize(cross(b - a, u.eye.xyz - p));
        const float half_width = 0.0015 + 0.0015 * distance(u.eye.xyz, p);
        AimOut out;
        out.position = u.clipFromOrigin * float4(p + side * (vid % 2 == 0 ? -half_width : half_width), 1.0);
        out.t = vid < 2 ? u.span.x : u.span.y;
        out.across = vid % 2 == 0 ? -1.0 : 1.0;
        return out;
    }
    struct MarkUniforms {
        float4x4 clipFromOrigin, eyeFromOrigin; float4 eye, at, color; float radius; uint count, mode, pad;
    };
    struct MarkOut { float4 position [[position]]; float2 q; float4 color; };
    // whether a point (origin space) is in front of the game's world: its distance ahead in the eye the picture was
    // drawn for against the game's depth there (as view_fragment turns and reads it); off the picture counts as in front
    static bool in_front(float3 p, constant MarkUniforms& u, constant ViewParams& v, texture2d<float> depth) {
        constexpr sampler nearest(filter::nearest, address::clamp_to_edge);
        const float3 e = (u.eyeFromOrigin * float4(p, 1.0)).xyz;
        const float3 pic = float3x3(v.turn0.xyz, v.turn1.xyz, v.turn2.xyz) * e;
        const float ahead = -pic.z;
        if (ahead < 0.05) return true;
        const float2 t = pic.xy / ahead;
        const float2 uv = float2((t.x - v.pictureEdges.x) / (v.pictureEdges.y - v.pictureEdges.x),
                                 (v.pictureEdges.z - t.y) / (v.pictureEdges.z - v.pictureEdges.w));
        if (any(uv < 0.0) || any(uv > 1.0)) return true;
        return ahead <= game_distance(depth.sample(nearest, uv).r, v) * 1.01 + 0.2;
    }
    // a round dot facing the eye: mode 0 at u.at; mode 1 where the arc (u.count points) first goes behind the world
    // after having been in front of it (the bow and the arms may hide its start), none when it never does
    vertex MarkOut mark_vertex(ushort vid [[vertex_id]], constant MarkUniforms& u [[buffer(0)]],
                               constant packed_float3* arc [[buffer(1)]], constant ViewParams& v [[buffer(2)]],
                               texture2d<float> depth [[texture(0)]]) {
        MarkOut out;
        out.color = u.color;
        out.q = float2(vid % 2 == 0 ? -1.0 : 1.0, vid < 2 ? -1.0 : 1.0);
        float3 c = u.at.xyz;
        if (u.mode == 1) {
            bool seen = false, found = false;
            for (uint k = 0; k < u.count && !found; k++) {
                const bool front = in_front(float3(arc[k]), u, v, depth);
                if (front) seen = true;
                else if (seen) { c = float3(arc[k]); found = true; }
            }
            if (!found) { out.position = float4(2.0, 2.0, 2.0, 1.0); return out; }  // off the screen
        }
        const float3 d = normalize(u.eye.xyz - c);
        const float3 side = normalize(cross(abs(d.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0), d));
        const float3 up = cross(d, side);
        const float r = max(u.radius, 0.006 * distance(u.eye.xyz, c));  // never smaller than about 0.35 degrees
        out.position = u.clipFromOrigin * float4(c + (side * out.q.x + up * out.q.y) * r, 1.0);
        return out;
    }
    fragment float4 mark_fragment(MarkOut in [[stage_in]]) {
        const float d = length(in.q);
        if (d > 1.0) discard_fragment();
        const float a = in.color.a * saturate((1.0 - d) * 6.0) * (0.7 + 0.3 * (1.0 - d));  // soft rim
        return float4(in.color.rgb * a, a);
    }
    fragment float4 aim_fragment(AimOut in [[stage_in]]) {
        const float edge = 1.0 - abs(in.across);                 // soft sides
        const float a = 0.8 * saturate(edge * 3.0) * (1.0 - 0.7 * in.t);  // fading towards the far end
        return float4(float3(1.0, 0.97, 0.9) * a, a);            // premultiplied, near white (BetterVR's arc)
    }
    fragment float4 hud_fragment(VertexOut in [[stage_in]], texture2d<float> hud [[texture(1)]]) {
        constexpr sampler s(filter::linear, address::clamp_to_edge);
        float4 h = hud.sample(s, in.uv);
        // premultiplied in the sRGB encoding: decode the colour un-premultiplied, premultiply again in linear
        float3 c = h.a > 0.0 ? decode(saturate(h.rgb / h.a)) * h.a : float3(0.0);
        return float4(c, h.a);
    }
    fragment float4 screen_fragment(VertexOut in [[stage_in]], constant Uniforms& u [[buffer(0)]],
                                    texture2d<float> eye [[texture(0)]], texture2d<float> hud [[texture(1)]]) {
        constexpr sampler s(filter::linear, address::clamp_to_edge);
        float3 c = eye.sample(s, in.uv).rgb;
        if (u.hasHud) {
            float4 h = hud.sample(s, in.uv);
            c = h.rgb + c * (1.0 - h.a);
        }
        return float4(decode(saturate(c)), 1.0);
    }
    """
}
