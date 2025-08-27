// Modules to control application life and create native browser window
const { app, BrowserWindow } = require('electron')

app.commandLine.appendSwitch('enable-gpu');
app.commandLine.appendSwitch('no-sandbox');

app.commandLine.appendSwitch('use-angle', 'gl-egl');

app.commandLine.appendSwitch('high-dpi-support', 1);
app.commandLine.appendSwitch('force-device-scale-factor', 1);

// Statistics for paint callbacks where texture is not null
const STATS_INTERVAL_MS = 3000;
let nonNullTexturePaintCount = 0;

setInterval(() => {
  const avgPerSecond = nonNullTexturePaintCount / (STATS_INTERVAL_MS / 1000);
  console.log(`paint(non-null) avg: ${avgPerSecond.toFixed(2)}/s over ${STATS_INTERVAL_MS / 1000}s (${nonNullTexturePaintCount} calls)`);
  nonNullTexturePaintCount = 0;
}, STATS_INTERVAL_MS);

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

  //osr.setBounds({ x: 0, y: 0, width, height });
  //osr.setSize(width, height);

  //osr.webContents.setFrameRate(60);
  //osr.webContents.invalidate();	

  osr.loadURL("https://app.singular.live/output/6W76ei5ZNekKkYhe8nw5o8/Output?aspect=16:9")
  osr.webContents.on('paint', (e, dirty, img) => {
    if (!e.texture) {
      console.log("skip null texture");
      return;
    }
    nonNullTexturePaintCount++;
    e.texture.release()
  })

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
  if (process.platform !== 'darwin') app.quit()
})

// In this file you can include the rest of your app's specific main process
// code. You can also put them in separate files and require them here.
