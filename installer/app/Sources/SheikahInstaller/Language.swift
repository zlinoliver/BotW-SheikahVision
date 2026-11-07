import Foundation
import SwiftUI

enum InstallerLanguageChoice: String, CaseIterable, Identifiable {
    case system, zh, en
    var id: String { rawValue }
    var label: String {
        switch self {
        case .system: return text("跟随系统", "System")
        case .zh: return "简体中文"
        case .en: return "English"
        }
    }
    static func resolve(saved: String?, environment: String?, preferred: String?) -> String {
        if let environment, ["zh", "en"].contains(environment) { return environment }
        if let saved, ["zh", "en"].contains(saved) { return saved }
        return preferred?.hasPrefix("zh") == true ? "zh" : "en"
    }
}

final class InstallerLanguage: ObservableObject {
    static let shared = InstallerLanguage()
    static let changed = Notification.Name("SheikahInstallerLanguageChanged")
    @Published private(set) var choice: InstallerLanguageChoice
    private let defaults: UserDefaults
    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        choice = InstallerLanguageChoice(rawValue: defaults.string(forKey: "installerLanguage") ?? "system") ?? .system
    }
    func select(_ value: InstallerLanguageChoice) {
        choice = value
        defaults.set(value.rawValue, forKey: "installerLanguage")
        NotificationCenter.default.post(name: Self.changed, object: self)
    }
}
