// What the Pi reports on GET /api/state. Only the fields the app uses are
// decoded; anything else in the reply is ignored.
import Foundation

struct PiState: Decodable {
    struct Bluetooth: Decodable {
        var live: Bool
        var device: String
        var error: String
    }
    struct Engine: Decodable {
        var ok: Bool
        var zones: [String]
        var level_db: [String: Double]
    }
    struct Device: Decodable {
        var name: String
        var outputs: Int
        var error: String
        var xruns: Int
    }
    struct Zone: Decodable {
        var freq: Double
        var level: Double
        var mute: Bool
    }
    struct Vibe: Decodable {
        var on: Bool
        var amount: Double
        var tone: Double
        var breath: Double
        var zones: [String: Zone]
    }
    struct Config: Decodable {
        var master: Double
        var mute: Bool
        var bed: String
        var vibe: Vibe
    }

    var live: Bool                 // AVA OS on a computer is streaming to the Pi
    var sender: String
    var bt: Bluetooth
    var engine: Engine
    var device: Device
    var out_db: [Double]
    var testing_zones: [String]
    var testing: [Int]
    var cfg: Config
}

/// One zone as shown and edited in the app.
struct ZoneSetting: Identifiable, Equatable {
    var name: String
    var level: Double
    var freq: Double
    var mute: Bool
    var id: String { name }
}

/// Meter position 0...1 for a level in dB (-60 dB and below reads as empty).
func meterFraction(_ db: Double) -> Double {
    return min(1.0, max(0.0, (db + 60.0) / 60.0))
}
