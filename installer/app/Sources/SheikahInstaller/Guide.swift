import SwiftUI

struct Guide: View {
    enum Kind { case pairing }
    let kind: Kind
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if kind == .pairing {
                // order as on the device: Developer Mode only shows up in Settings after the headset has been paired
                Label(text("1. 头显：设置 › 通用 › 远程设备", "1. Vision Pro: Settings › General › Remote Devices"), systemImage: "visionpro")
                Label(text("2. Mac：Xcode Device Hub › + › Pair Nearby Device，输入配对码", "2. Mac: Xcode Device Hub › + › Pair Nearby Device; enter the code"), systemImage: "laptopcomputer")
                Label(text("3. 头显：设置 › 隐私与安全性 › 开发者模式，打开并重启", "3. Vision Pro: Settings › Privacy & Security › Developer Mode; turn on, restart"), systemImage: "gearshape")
            }
        }.font(.system(size: 12)).fixedSize(horizontal: false, vertical: true)
            .padding(12).frame(maxWidth: .infinity, alignment: .leading)
            .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 14))
    }
}
