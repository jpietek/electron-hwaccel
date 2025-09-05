// Modules to control application life and create native browser window
const { app, BrowserWindow } = require('electron')
const fs = require('node:fs')
const net = require('node:net')
const path = require('node:path')

const UDS_PATH = '/tmp/electron-hwaccel.sock'
let udsSocket = null
let udsConnectPromise = null

let fdpass = null
try {
  fdpass = require('fdpass')
} catch (e) {
  console.warn('fdpass addon not available; falling back to JSON-only payloads')
}

let paintCount = 0
let lastStatsTime = Date.now()
let statsInterval = null

async function ensureUdsConnected () {
  if (udsSocket && !udsSocket.destroyed) return udsSocket
  if (udsConnectPromise) return udsConnectPromise

  udsConnectPromise = new Promise((resolve, reject) => {
    const socket = net.createConnection(UDS_PATH)
    const onError = (err) => {
      cleanup()
      reject(err)
    }
    const onConnect = () => {
      cleanup()
      udsSocket = socket
      resolve(socket)
    }
    const cleanup = () => {
      socket.off('error', onError)
      socket.off('connect', onConnect)
    }
    socket.once('error', onError)
    socket.once('connect', onConnect)
  }).finally(() => { udsConnectPromise = null })

  return udsConnectPromise
}

app.commandLine.appendSwitch('enable-gpu');
app.commandLine.appendSwitch('no-sandbox');

app.commandLine.appendSwitch('use-angle', 'gl-egl');

app.commandLine.appendSwitch('high-dpi-support', 1);
app.commandLine.appendSwitch('force-device-scale-factor', 1);

function createWindow () {

  const width = 1920;
  const height = 1080;

  const osr = new BrowserWindow({
    width,
    height,
    webPreferences: {
      offscreen: {
        useSharedTexture: true
      },
      sandbox: false,
      show: false
    }
  })

  osr.setBounds({ x: 0, y: 0, width, height });
  osr.setSize(width, height);

  osr.webContents.setFrameRate(60);
  osr.webContents.invalidate();	

  osr.loadURL("https://app.singular.live/output/6W76ei5ZNekKkYhe8nw5o8/Output?aspect=16:9")
  osr.webContents.on('paint', async (e, dirty, img) => {
    paintCount++
    try {
      const texJson = typeof e.texture?.toJSON === 'function' ? e.texture.toJSON() : e.texture
      const fd = texJson?.textureInfo?.planes?.[0]?.fd
      const socketPath = UDS_PATH
      if (fdpass && typeof fd === 'number') {
        try {
          await ensureUdsConnected()
          await fdpass.sendJsonWithFds(socketPath, texJson, [fd])
        } catch (err) {
          console.error('sendJsonWithFds failed:', err)
        }
      }
    } catch (err) {
      console.error('exception:', err);
    } finally {
      e.texture.release()
    }
  })

  // Start paint statistics reporting
  statsInterval = setInterval(() => {
    const now = Date.now()
    const elapsed = (now - lastStatsTime) / 1000 // seconds
    const paintsPerSecond = paintCount / elapsed
    console.log(`Paint stats: ${paintCount} paints in ${elapsed.toFixed(1)}s = ${paintsPerSecond.toFixed(1)} paints/sec`)
    paintCount = 0
    lastStatsTime = now
  }, 3000)
}

app.whenReady().then(() => {
  createWindow()

  app.on('activate', function () {

    if (BrowserWindow.getAllWindows().length === 0) createWindow()
  })
})

app.on('window-all-closed', function () {
  if (statsInterval) {
    clearInterval(statsInterval)
  }
  if (process.platform !== 'darwin') app.quit()
})
