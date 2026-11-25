// AVA OS for iPhone and iPad.
// Two tabs: BED is the remote for the bed controller (the Raspberry Pi running
// AVA Air), PLAY is the on-device engine with the octagon pads.
import SwiftUI

@main
struct AVAOSApp: App {
    @StateObject private var pi = PiClient()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(pi)
                .preferredColorScheme(.dark)
        }
    }
}

struct RootView: View {
    @AppStorage("tab") private var tab = "bed"

    var body: some View {
        TabView(selection: $tab) {
            BedView()
                .tabItem { Label("Bed", systemImage: "dot.radiowaves.left.and.right") }
                .tag("bed")
            PlayView()
                .tabItem { Label("Play", systemImage: "octagon") }
                .tag("play")
        }
        .tint(.white)
    }
}
