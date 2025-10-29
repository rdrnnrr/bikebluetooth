import SwiftUI
import Combine

struct ContentView: View {
    @EnvironmentObject private var bluetoothManager: BluetoothManager
    @EnvironmentObject private var nowPlayingManager: NowPlayingManager

    var body: some View {
        NavigationView {
            VStack(alignment: .leading, spacing: 24) {
                connectionSection
                nowPlayingSection
                Spacer()
            }
            .padding()
            .navigationTitle("JuiceBox")
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Button(action: bluetoothManager.toggleConnection) {
                        Label(bluetoothManager.isConnected ? "Disconnect" : "Connect",
                              systemImage: bluetoothManager.isConnected ? "bolt.horizontal.circle.fill" : "dot.radiowaves.left.and.right")
                    }
                    .disabled(bluetoothManager.isBusy)
                }
            }
        }
        .onReceive(nowPlayingManager.$currentSong.removeDuplicates()) { song in
            guard bluetoothManager.isConnected else { return }
            bluetoothManager.send(song: BluetoothManager.SongPayload(from: song))
        }
        .onChange(of: bluetoothManager.isConnected) { isConnected in
            guard isConnected else { return }
            let song = nowPlayingManager.currentSong
            guard !song.title.isEmpty else { return }
            bluetoothManager.send(song: BluetoothManager.SongPayload(from: song), force: true)
        }
    }

    private var connectionSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Remote Status")
                .font(.headline)
            HStack {
                Circle()
                    .fill(bluetoothManager.isConnected ? Color.green : Color.red)
                    .frame(width: 12, height: 12)
                Text(bluetoothManager.connectionDescription)
                    .font(.subheadline)
                    .foregroundColor(.secondary)
            }
            if let errorMessage = bluetoothManager.errorMessage {
                Text(errorMessage)
                    .font(.caption)
                    .foregroundColor(.red)
            }
        }
    }

    private var nowPlayingSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Now Playing")
                .font(.headline)

            VStack(alignment: .leading, spacing: 4) {
                Text(nowPlayingManager.currentSong.title.isEmpty ? "—" : nowPlayingManager.currentSong.title)
                    .font(.title2)
                    .bold()
                Text(nowPlayingManager.currentSong.artist.isEmpty ? "Unknown Artist" : nowPlayingManager.currentSong.artist)
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                Text(nowPlayingManager.currentSong.album.isEmpty ? "Unknown Album" : nowPlayingManager.currentSong.album)
                    .font(.caption)
                    .foregroundColor(.secondary)
            }

            Button(action: sendCurrentSong) {
                Label("Send to Remote", systemImage: "paperplane.fill")
            }
            .buttonStyle(.borderedProminent)
            .disabled(!bluetoothManager.canSend || nowPlayingManager.currentSong.title.isEmpty)
        }
    }

    private func sendCurrentSong() {
        bluetoothManager.send(song: BluetoothManager.SongPayload(from: nowPlayingManager.currentSong), force: true)
    }
}

private extension BluetoothManager.SongPayload {
    init(from song: NowPlayingManager.Song) {
        self.init(artist: song.artist, album: song.album, title: song.title)
    }
}

struct ContentView_Previews: PreviewProvider {
    static var previews: some View {
        ContentView()
            .environmentObject(BluetoothManager(preview: true))
            .environmentObject(NowPlayingManager(preview: true))
    }
}
