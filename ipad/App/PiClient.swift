// Talks to the Pi over Wi-Fi: polls its status twice a second and sends
// setting changes to POST /api/config.
import Foundation
import SwiftUI

@MainActor
final class PiClient: ObservableObject {
    static let defaultHost = "danepi1.local"

    // connection
    @Published var host: String
    @Published var connected = false
    @Published var lastError = ""
    @Published var state: PiState?

    // settings, as shown on screen
    @Published var master: Double = 0.5
    @Published var mute = false
    @Published var bed = "octagon5"
    @Published var engineOn = true
    @Published var amount: Double = 1.0
    @Published var tone: Double = 0.6
    @Published var breath: Double = 0.0
    @Published var zones: [ZoneSetting] = []

    private var pollTask: Task<Void, Never>?
    private var sendTask: Task<Void, Never>?
    private var pending: [String: Any] = [:]
    private var holdUntil = Date.distantPast   // ignore the Pi's values briefly after a local change
    private let session: URLSession

    init() {
        host = UserDefaults.standard.string(forKey: "piHost") ?? PiClient.defaultHost
        let cfg = URLSessionConfiguration.ephemeral
        cfg.timeoutIntervalForRequest = 2.5
        cfg.waitsForConnectivity = false
        session = URLSession(configuration: cfg)
    }

    // MARK: connection

    func setHost(_ newHost: String) {
        let trimmed = newHost.trimmingCharacters(in: .whitespacesAndNewlines)
        host = trimmed.isEmpty ? PiClient.defaultHost : trimmed
        UserDefaults.standard.set(host, forKey: "piHost")
        connected = false
        state = nil
        start()
    }

    func start() {
        pollTask?.cancel()
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                await self?.poll()
                try? await Task.sleep(nanoseconds: 500_000_000)
            }
        }
    }

    func stop() {
        pollTask?.cancel()
        pollTask = nil
    }

    private func url(_ path: String) -> URL? {
        var h = host
        if h.hasPrefix("http://") { h = String(h.dropFirst(7)) }
        if h.hasSuffix("/") { h = String(h.dropLast()) }
        let hasPort = h.contains(":")
        return URL(string: "http://" + h + (hasPort ? "" : ":8080") + path)
    }

    private func poll() async {
        guard let u = url("/api/state") else {
            connected = false
            lastError = "That address does not look right."
            return
        }
        do {
            let (data, _) = try await session.data(from: u)
            let s = try JSONDecoder().decode(PiState.self, from: data)
            state = s
            connected = true
            lastError = ""
            if Date() > holdUntil { adopt(s.cfg, order: s.engine.zones) }
        } catch is CancellationError {
            return
        } catch let e as DecodingError {
            connected = false
            lastError = "The Pi answered, but not in the expected form. \(e)"
        } catch {
            connected = false
            lastError = error.localizedDescription
        }
    }

    private func adopt(_ c: PiState.Config, order: [String]) {
        master = c.master
        mute = c.mute
        bed = c.bed
        engineOn = c.vibe.on
        amount = c.vibe.amount
        tone = c.vibe.tone
        breath = c.vibe.breath
        var list: [ZoneSetting] = []
        for name in order {
            if let z = c.vibe.zones[name] {
                list.append(ZoneSetting(name: name, level: z.level, freq: z.freq, mute: z.mute))
            }
        }
        if list != zones { zones = list }
    }

    // MARK: sending changes

    /// Queue a change for /api/config. Changes made close together are merged
    /// and sent as one request, so dragging a slider does not flood the Pi.
    func change(_ patch: [String: Any]) {
        holdUntil = Date().addingTimeInterval(1.5)
        pending = PiClient.merged(pending, patch)
        if sendTask != nil { return }
        sendTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 120_000_000)
            guard let self = self else { return }
            let body = self.pending
            self.pending = [:]
            self.sendTask = nil
            await self.post("/api/config", body)
        }
    }

    func setMaster(_ v: Double) { master = v; change(["master": v]) }
    func setMute(_ v: Bool) { mute = v; change(["mute": v]) }
    func setBed(_ v: String) { bed = v; change(["bed": v]) }
    func setEngineOn(_ v: Bool) { engineOn = v; change(["vibe": ["on": v]]) }
    func setAmount(_ v: Double) { amount = v; change(["vibe": ["amount": v]]) }
    func setTone(_ v: Double) { tone = v; change(["vibe": ["tone": v]]) }
    func setBreath(_ v: Double) { breath = v; change(["vibe": ["breath": v]]) }

    func setZone(_ name: String, level: Double? = nil, freq: Double? = nil, mute: Bool? = nil) {
        guard let i = zones.firstIndex(where: { $0.name == name }) else { return }
        var z: [String: Any] = [:]
        if let v = level { zones[i].level = v; z["level"] = v }
        if let v = freq { zones[i].freq = v; z["freq"] = v }
        if let v = mute { zones[i].mute = v; z["mute"] = v }
        change(["vibe": ["zones": [name: z]]])
    }

    /// Pulse one zone for a moment so it can be found by feel.
    func pulseZone(_ name: String, seconds: Double = 2.0) {
        Task { await post("/api/test", ["zone": name, "seconds": seconds]) }
    }

    /// Test tone on one sound card output (0-based).
    func testOutput(_ index: Int, seconds: Double = 2.0) {
        Task { await post("/api/test", ["out": index, "seconds": seconds]) }
    }

    func stopTests() {
        Task { await post("/api/stoptest", [:]) }
    }

    private func post(_ path: String, _ body: [String: Any]) async {
        guard let u = url(path),
              let data = try? JSONSerialization.data(withJSONObject: body) else { return }
        var req = URLRequest(url: u)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = data
        do {
            _ = try await session.data(for: req)
        } catch {
            lastError = error.localizedDescription
        }
    }

    /// Merge b into a, going into nested dictionaries (vibe > zones > HEAD ...).
    static func merged(_ a: [String: Any], _ b: [String: Any]) -> [String: Any] {
        var out = a
        for (k, v) in b {
            if let old = out[k] as? [String: Any], let new = v as? [String: Any] {
                out[k] = merged(old, new)
            } else {
                out[k] = v
            }
        }
        return out
    }
}
