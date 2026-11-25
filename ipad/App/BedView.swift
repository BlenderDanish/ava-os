// AVA OS - BED tab: the remote for the bed controller (the Pi running AVA Air).
// Status, master level, the Pi's engine and its zones, and test tones for
// finding each output by feel.
import SwiftUI

struct BedView: View {
    @EnvironmentObject var pi: PiClient
    @Environment(\.scenePhase) private var scenePhase
    @State private var showSettings = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 14) {
                    StatusCard()
                    if pi.connected {
                        MasterCard()
                        EngineCard()
                        ZonesCard()
                        OutputsCard()
                    } else {
                        NotConnectedCard(showSettings: $showSettings)
                    }
                }
                .padding(16)
            }
            .background(Color.black.ignoresSafeArea())
            .navigationTitle("AVA")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button {
                        showSettings = true
                    } label: {
                        Image(systemName: "gearshape")
                    }
                }
            }
            .sheet(isPresented: $showSettings) {
                SettingsSheet()
            }
        }
        .tint(.white)
        .onAppear { pi.start() }
        .onChange(of: scenePhase) { phase in
            if phase == .active { pi.start() } else { pi.stop() }
        }
    }
}

// MARK: - cards

struct Card<Content: View>: View {
    let title: String
    let content: Content

    init(_ title: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if !title.isEmpty {
                Text(title)
                    .font(.system(size: 12, weight: .semibold, design: .monospaced))
                    .foregroundColor(.white.opacity(0.5))
            }
            content
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.white.opacity(0.07))
        .cornerRadius(14)
    }
}

struct StatusCard: View {
    @EnvironmentObject var pi: PiClient

    var body: some View {
        Card("") {
            HStack(spacing: 10) {
                Circle()
                    .fill(pi.connected ? Color.green : Color.orange)
                    .frame(width: 10, height: 10)
                Text(pi.connected ? "Connected to \(pi.host)" : "Looking for \(pi.host)")
                    .font(.system(size: 15, weight: .medium))
                Spacer()
            }
            if let s = pi.state, pi.connected {
                VStack(alignment: .leading, spacing: 4) {
                    Text(sourceLine(s))
                    Text(deviceLine(s))
                }
                .font(.system(size: 13))
                .foregroundColor(.white.opacity(0.6))
            }
        }
    }

    private func sourceLine(_ s: PiState) -> String {
        var parts: [String] = []
        if s.bt.live { parts.append("Phone music" + (s.bt.device.isEmpty ? "" : " from \(s.bt.device)")) }
        if s.live { parts.append("AVA OS from \(s.sender)") }
        return parts.isEmpty ? "No music coming in" : parts.joined(separator: "  ·  ")
    }

    private func deviceLine(_ s: PiState) -> String {
        if !s.device.error.isEmpty { return "Sound card: \(s.device.error)" }
        return "\(s.device.name)"
    }
}

struct NotConnectedCard: View {
    @EnvironmentObject var pi: PiClient
    @Binding var showSettings: Bool

    var body: some View {
        Card("NOT CONNECTED") {
            Text("Make sure this phone is on the same Wi-Fi as the bed and the bed is powered on.")
                .font(.system(size: 14))
                .foregroundColor(.white.opacity(0.75))
            if !pi.lastError.isEmpty {
                Text(pi.lastError)
                    .font(.system(size: 12))
                    .foregroundColor(.white.opacity(0.45))
            }
            Button("Change the bed's address") { showSettings = true }
                .buttonStyle(.bordered)
        }
    }
}

struct MasterCard: View {
    @EnvironmentObject var pi: PiClient

    var body: some View {
        Card("MASTER") {
            HStack(spacing: 12) {
                Slider(value: Binding(get: { pi.master }, set: { pi.setMaster($0) }), in: 0...2)
                Text("\(Int((pi.master * 100).rounded()))%")
                    .font(.system(size: 14, design: .monospaced))
                    .frame(width: 52, alignment: .trailing)
            }
            Toggle("Mute everything", isOn: Binding(get: { pi.mute }, set: { pi.setMute($0) }))
        }
    }
}

struct EngineCard: View {
    @EnvironmentObject var pi: PiClient

    var body: some View {
        Card("VIBRATION FROM PHONE MUSIC") {
            Toggle("On", isOn: Binding(get: { pi.engineOn }, set: { pi.setEngineOn($0) }))
            LabeledSlider(label: "Strength", value: Binding(get: { pi.amount }, set: { pi.setAmount($0) }),
                          range: 0...3, text: String(format: "%.2f", pi.amount))
            LabeledSlider(label: "Tone", value: Binding(get: { pi.tone }, set: { pi.setTone($0) }),
                          range: 0...1, text: String(format: "%.2f", pi.tone))
            LabeledSlider(label: "Breath", value: Binding(get: { pi.breath }, set: { pi.setBreath($0) }),
                          range: 0...1, text: pi.breath < 0.005 ? "off" : String(format: "%.2f", pi.breath))
            Text("Tone: 0 follows the music's own bass, 1 plays only the tuned tones.")
                .font(.system(size: 12))
                .foregroundColor(.white.opacity(0.45))
        }
    }
}

struct ZonesCard: View {
    @EnvironmentObject var pi: PiClient

    var body: some View {
        Card("ZONES") {
            ForEach(pi.zones) { zone in
                ZoneRow(zone: zone)
                if zone.id != pi.zones.last?.id {
                    Divider().background(Color.white.opacity(0.15))
                }
            }
        }
    }
}

struct ZoneRow: View {
    @EnvironmentObject var pi: PiClient
    let zone: ZoneSetting

    var body: some View {
        let db = pi.state?.engine.level_db[zone.name] ?? -100.0
        let pulsing = pi.state?.testing_zones.contains(zone.name) ?? false
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 10) {
                Text(zone.name)
                    .font(.system(size: 15, weight: .semibold, design: .monospaced))
                    .frame(width: 64, alignment: .leading)
                MeterBar(fraction: zone.mute ? 0 : meterFraction(db))
                Button(zone.mute ? "Muted" : "Mute") {
                    pi.setZone(zone.name, mute: !zone.mute)
                }
                .buttonStyle(.bordered)
                .tint(zone.mute ? .orange : .white)
                Button(pulsing ? "..." : "Pulse") {
                    pi.pulseZone(zone.name)
                }
                .buttonStyle(.bordered)
            }
            LabeledSlider(label: "Level",
                          value: Binding(get: { zone.level }, set: { pi.setZone(zone.name, level: $0) }),
                          range: 0...3, text: String(format: "%.2f", zone.level))
            LabeledSlider(label: "Tuned",
                          value: Binding(get: { zone.freq }, set: { pi.setZone(zone.name, freq: ($0).rounded()) }),
                          range: 15...250, text: "\(Int(zone.freq.rounded())) Hz")
        }
    }
}

struct OutputsCard: View {
    @EnvironmentObject var pi: PiClient

    var body: some View {
        let count = pi.state?.device.outputs ?? 0
        let levels = pi.state?.out_db ?? []
        let testing = pi.state?.testing ?? []
        Card("OUTPUTS") {
            if count == 0 {
                Text("No sound card is connected to the bed controller.")
                    .font(.system(size: 14))
                    .foregroundColor(.white.opacity(0.6))
            } else {
                ForEach(0..<count, id: \.self) { i in
                    HStack(spacing: 10) {
                        Text("Out \(i + 1)")
                            .font(.system(size: 14, design: .monospaced))
                            .frame(width: 64, alignment: .leading)
                        MeterBar(fraction: i < levels.count ? meterFraction(levels[i]) : 0)
                        Button(testing.contains(i) ? "..." : "Test") {
                            pi.testOutput(i)
                        }
                        .buttonStyle(.bordered)
                    }
                }
                Button("Stop all test tones") { pi.stopTests() }
                    .buttonStyle(.bordered)
                Text("Test plays a short low tone on one output so you can find its transducers by feel.")
                    .font(.system(size: 12))
                    .foregroundColor(.white.opacity(0.45))
            }
        }
    }
}

// MARK: - small pieces

struct LabeledSlider: View {
    let label: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let text: String

    var body: some View {
        HStack(spacing: 10) {
            Text(label)
                .font(.system(size: 14))
                .foregroundColor(.white.opacity(0.75))
                .frame(width: 64, alignment: .leading)
            Slider(value: $value, in: range)
            Text(text)
                .font(.system(size: 13, design: .monospaced))
                .frame(width: 58, alignment: .trailing)
        }
    }
}

struct MeterBar: View {
    let fraction: Double

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(Color.white.opacity(0.12))
                Capsule()
                    .fill(Color.white.opacity(0.85))
                    .frame(width: max(0, geo.size.width * CGFloat(fraction)))
            }
        }
        .frame(height: 8)
        .animation(.linear(duration: 0.2), value: fraction)
    }
}

struct SettingsSheet: View {
    @EnvironmentObject var pi: PiClient
    @Environment(\.dismiss) private var dismiss
    @State private var address = ""
    private let suggestions = ["danepi1.local", "ava-pi.local", "192.168.1.216", "192.168.1.8"]

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField(PiClient.defaultHost, text: $address)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled(true)
                        .keyboardType(.URL)
                } header: {
                    Text("Bed address")
                } footer: {
                    Text("The name or IP address of the bed controller. The phone must be on the same Wi-Fi as the bed.")
                }
                Section {
                    ForEach(suggestions, id: \.self) { s in
                        Button(s) { address = s }
                    }
                } header: {
                    Text("Try one of these")
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        pi.setHost(address)
                        dismiss()
                    }
                }
            }
            .onAppear { address = pi.host }
        }
        .preferredColorScheme(.dark)
    }
}
