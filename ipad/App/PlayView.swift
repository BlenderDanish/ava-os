// AVA OS - PLAY tab: the on-device engine. Octagon pads plus the essential
// params; the microphone follows the music in the room.
import SwiftUI
import AVFoundation
import UIKit

struct PlayView: View {
    @Environment(\.horizontalSizeClass) private var hSize
    @AppStorage("wiring") private var wiring = 0
    @State private var intensity: Double = 0.7
    @State private var voidAmt: Double = 0.0
    @State private var master: Double = 1.0
    @State private var patch: Int = 0
    @State private var bpm: Float = 120
    @State private var rootHz: Float = 55
    @State private var level: Float = 0
    @State private var route = "-"
    @State private var channels = 0
    @State private var activeWiring = 0
    @State private var started = false
    @State private var starting = false
    @State private var errorText = ""

    private let patches = ["PURE", "WHALE", "QUAKE", "HEART", "PURR", "DROP"]
    private let wiringTags = [0, 8, 4, 2]
    private let poll = Timer.publish(every: 0.25, on: .main, in: .common).autoconnect()

    private var compact: Bool { hSize == .compact }
    private var isPad: Bool { UIDevice.current.userInterfaceIdiom == .pad }

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            VStack(spacing: compact ? 10 : 14) {
                header

                OctagonPadView()
                    .aspectRatio(1, contentMode: .fit)
                    .frame(maxWidth: 560)
                    .opacity(started ? 1 : 0.4)
                    .allowsHitTesting(started)
                    .padding(.horizontal, compact ? 8 : 0)

                Text(String(format: "%.0f BPM   ·   %.0f Hz", bpm, rootHz))
                    .font(.system(size: 14, design: .monospaced))
                    .foregroundColor(.white.opacity(0.55))

                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 8) {
                        ForEach(patches.indices, id: \.self) { i in
                            Button(patches[i]) {
                                patch = i
                                AVABridge.shared().setPatch(Int32(i))
                            }
                            .font(.system(size: 13, weight: .medium, design: .monospaced))
                            .foregroundColor(patch == i ? .black : .white.opacity(0.7))
                            .padding(.horizontal, 14).padding(.vertical, 8)
                            .background(patch == i ? Color.white : Color.white.opacity(0.08))
                            .cornerRadius(16)
                        }
                    }
                    .padding(.horizontal, sidePad)
                }

                VStack(spacing: compact ? 4 : 10) {
                    paramSlider("INTENSITY", $intensity) { AVABridge.shared().setIntensity(Float($0)) }
                    paramSlider("VOID", $voidAmt) { AVABridge.shared().setVoidAmt(Float($0)) }
                    paramSlider("MASTER", $master) { AVABridge.shared().setMaster(Float($0)) }
                }
                .frame(maxWidth: 560)

                wiringRow
                    .frame(maxWidth: 560)

                if !errorText.isEmpty {
                    Text(errorText).font(.system(size: 12, design: .monospaced))
                        .foregroundColor(.red.opacity(0.8))
                        .padding(.horizontal, sidePad)
                }
            }
            .padding(.bottom, 10)
        }
        .statusBarHidden(isPad)
        .onAppear {
            AVABridge.shared().setWiring(Int32(wiring))
            // the iPad is a dedicated surface and starts listening right away;
            // on a phone the engine waits for START, so opening the app never
            // takes the audio away from music that is already playing
            if isPad && !started { startEngine() }
        }
        .onChange(of: wiring) { AVABridge.shared().setWiring(Int32($0)) }
        .onReceive(poll) { _ in
            let b = AVABridge.shared()
            let run = b.isRunning()
            if started != run && !starting {
                started = run
                UIApplication.shared.isIdleTimerDisabled = run
            }
            guard run else { return }
            bpm = b.bpm(); rootHz = b.rootHz(); level = b.level()
            route = b.outputName(); channels = Int(b.outputChannels())
            activeWiring = Int(b.activeWiring())
        }
    }

    private var sidePad: CGFloat { compact ? 16 : 26 }

    private var header: some View {
        HStack(spacing: 8) {
            Text("AVA OS").font(.system(size: compact ? 18 : 22, weight: .semibold, design: .monospaced))
                .foregroundColor(.white.opacity(0.95))
            Spacer(minLength: 4)
            Circle().fill(started && level > 0.02 ? Color.white : Color.white.opacity(0.2))
                .frame(width: 9, height: 9)
            Text(started ? "\(route) · \(channels)ch" : (starting ? "starting…" : "stopped"))
                .font(.system(size: 13, design: .monospaced))
                .foregroundColor(.white.opacity(0.45))
                .lineLimit(1)
                .truncationMode(.middle)
            Button(started ? "STOP" : "START") {
                if started { stopEngine() } else { startEngine() }
            }
            .font(.system(size: 13, weight: .semibold, design: .monospaced))
            .foregroundColor(started ? .white.opacity(0.8) : .black)
            .padding(.horizontal, 14).padding(.vertical, 7)
            .background(started ? Color.white.opacity(0.12) : Color.white)
            .cornerRadius(16)
            .disabled(starting)
        }
        .padding(.horizontal, sidePad).padding(.top, compact ? 8 : 14)
    }

    private var wiringRow: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 14) {
                Text("WIRING").font(.system(size: 11, design: .monospaced))
                    .foregroundColor(.white.opacity(0.4)).frame(width: 84, alignment: .leading)
                Picker("Wiring", selection: $wiring) {
                    ForEach(wiringTags, id: \.self) { t in
                        Text(t == 0 ? "ZONES" : "\(t) CH").tag(t)
                    }
                }
                .pickerStyle(.segmented)
            }
            Text(wiringCaption)
                .font(.system(size: 11, design: .monospaced))
                .foregroundColor(.white.opacity(0.4))
                .lineLimit(2)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.horizontal, sidePad)
    }

    private var wiringCaption: String {
        let shown = started ? activeWiring : wiring
        var text: String
        switch shown {
        case 8: text = "circle: one channel per transducer on Ch 3-10"
        case 4: text = "circle: Ch 1 + 4 outer ring, Ch 2 + 3 inner disc"
        case 2: text = "circle: left = inner disc, right = outer ring"
        default: text = channels >= 7 || !started
            ? "octagon: one channel per body zone"
            : "every zone summed on both channels"
        }
        if started && shown != wiring {
            text += " (this output has only \(channels) ch)"
        }
        return text
    }

    private func startEngine() {
        starting = true
        errorText = ""
        AVAudioSessionHelper.requestMic { granted in
            let b = AVABridge.shared()
            b.setWiring(Int32(wiring))
            b.setIntensity(Float(intensity))
            b.setVoidAmt(Float(voidAmt))
            b.setMaster(Float(master))
            let ok = b.start()
            started = ok
            starting = false
            UIApplication.shared.isIdleTimerDisabled = ok
            if !ok {
                errorText = b.lastError
            } else if !granted {
                errorText = "microphone denied: pads only"
            }
        }
    }

    private func stopEngine() {
        AVABridge.shared().stop()
        started = false
        level = 0
        UIApplication.shared.isIdleTimerDisabled = false
    }

    private func paramSlider(_ label: String, _ v: Binding<Double>,
                             onChange: @escaping (Double) -> Void) -> some View {
        HStack(spacing: 14) {
            Text(label).font(.system(size: 11, design: .monospaced))
                .foregroundColor(.white.opacity(0.4)).frame(width: 84, alignment: .leading)
            Slider(value: v).tint(.white.opacity(0.85))
                .onChange(of: v.wrappedValue) { onChange($0) }
            Text(String(format: "%.0f", v.wrappedValue * 100))
                .font(.system(size: 11, design: .monospaced))
                .foregroundColor(.white.opacity(0.4)).frame(width: 30, alignment: .trailing)
        }
        .padding(.horizontal, sidePad)
    }
}

enum AVAudioSessionHelper {
    static func requestMic(_ done: @escaping (Bool) -> Void) {
        AVAudioSession.sharedInstance().requestRecordPermission { g in
            DispatchQueue.main.async { done(g) }
        }
    }
}
