// Modules to control application life and create native browser window
const { app, BrowserWindow } = require('electron')
const path = require('node:path')
const zmq = require('zeromq')

// ZeroMQ REQ client setup
const ZMQ_ENDPOINT = 'tcp://127.0.0.1:5555'
const zmqClient = new zmq.Request()
let zmqConnectPromise = null
let zmqQueue = Promise.resolve()

// Paint statistics
let paintCount = 0
let lastStatsTime = Date.now()
let statsInterval = null

async function ensureZmqConnected () {
  if (!zmqConnectPromise) {
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

function enqueueZmqSend (payload) {
  const task = async () => {
    await ensureZmqConnected()
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

  // Create the browser window.
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
      await enqueueZmqSend(e.texture)
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
    console.log(`Paint stats: ${paintCount} paints in ${elapsed.toFixed(1)}s = ${paintsPerSecond.toFixed(1)} paints/sec`)
    paintCount = 0
    lastStatsTime = now
  }, 3000)

  //osr.webContents.openDevTools()
}

// This method will be called when Electron has finished
// initialization and is ready to create browser windows.
// Some APIs can only be used after this event occurs.
app.whenReady().then(() => {
  createWindow()

  app.on('activate', function () {
    // On macOS it's common to re-create a window in the app when the
    // dock icon is clicked and there are no other windows open.
    if (BrowserWindow.getAllWindows().length === 0) createWindow()
  })
})

// Quit when all windows are closed, except on macOS. There, it's common
// for applications and their menu bar to stay active until the user quits
// explicitly with Cmd + Q.
app.on('window-all-closed', function () {
  if (statsInterval) {
    clearInterval(statsInterval)
  }
  if (process.platform !== 'darwin') app.quit()
})

// In this file you can include the rest of your app's specific main process
// code. You can also put them in separate files and require them here.
