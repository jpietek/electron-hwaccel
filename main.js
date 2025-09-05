// Modules to control application life and create native browser window
const { app, BrowserWindow } = require('electron')
const fs = require('node:fs')
const zmq = require('zeromq')

const ZMQ_ENDPOINT = 'tcp://127.0.0.1:5555'
const zmqClient = new zmq.Request()
let zmqConnectPromise = null
let zmqQueue = Promise.resolve()
const connectedEndpoints = new Set()
let eventsLoopStarted = false

let fdpass = null
try {
  fdpass = require('fdpass')
} catch (e) {
  console.warn('fdpass addon not available; falling back to JSON-only payloads')
}

let paintCount = 0
let lastStatsTime = Date.now()
let statsInterval = null

async function ensureZmqConnected () {
  if (!zmqConnectPromise) {
    if (!eventsLoopStarted) {
      eventsLoopStarted = true
      ;(async () => {
        try {
          const eventsSource = zmqClient.events
          if (eventsSource && typeof eventsSource[Symbol.asyncIterator] === 'function') {
            for await (const ev of eventsSource) {
              const type = ev && (ev.type || ev.event || ev[0])
              const address = ev && (ev.address || ev.addr || ev.endpoint || ev[1])
              if (type === 'connect') connectedEndpoints.add(address)
              else if (type === 'disconnect') connectedEndpoints.delete(address)
            }
          }
        } catch (err) {
          console.error('ZMQ events error:', err)
        }
      })()
    }
    zmqConnectPromise = (async () => {
      await zmqClient.connect(ZMQ_ENDPOINT)
    })().catch(err => {
      console.error('ZMQ connect error:', err)
      zmqConnectPromise = null
      throw err
    })
  }
  return zmqConnectPromise
}

function hasPeer () {
  return connectedEndpoints.size > 0
}

function enqueueZmqSend (payload) {
  const task = async () => {
    await ensureZmqConnected()
    if (!hasPeer()) {
      return undefined
    }
    const message = typeof payload === 'string' ? payload : JSON.stringify(payload)
    await zmqClient.send(message)
    const [reply] = await zmqClient.receive()
    return reply
  }
  const next = zmqQueue.then(task, task)
  zmqQueue = next.catch(() => {})
  return next
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
      if (fdpass && typeof fd === 'number') {
        const fdPassSocketPath = '/tmp/' + fd + '.sock'
        try {
          if (!fs.existsSync(fdPassSocketPath)) {
            // Skip until receiver/socket exists
          } else {
            await fdpass.sendFd(fdPassSocketPath, fd)
            if (texJson?.textureInfo?.planes?.[0]) texJson.textureInfo.planes[0].fd = -1
          }
        } catch (err) {
          console.error('fdpass sendFd failed:', err)
        }
      }

      await enqueueZmqSend(texJson)
    } catch (err) {
      console.error('ZMQ send/recv failed:', err)
    } finally {
      e.texture.release()
    }
  })

  // Start paint statistics reporting
  statsInterval = setInterval(() => {
    const now = Date.now()
    const elapsed = (now - lastStatsTime) / 1000 // seconds
    const paintsPerSecond = paintCount / elapsed
    console.log(`Paint stats: ${paintCount} paints in ${elapsed.toFixed(1)}s = ${paintsPerSecond.toFixed(1)} paints/sec, peers=${connectedEndpoints.size}`)
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
