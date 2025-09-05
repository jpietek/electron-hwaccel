'use strict'

const path = require('node:path')
const binary = require('node-gyp-build')

const addon = binary(path.join(__dirname))

async function sendFd (socketPath, fd) {
  // Wrap in a promise to keep async/await style at call-site
  return new Promise((resolve, reject) => {
    try {
      addon.sendFd(socketPath, fd)
      resolve()
    } catch (err) {
      reject(err)
    }
  })
}

function createEGLImageFromDMABuf (opts) {
  // Returns a BigInt representing the EGLImageKHR handle
  return addon.createEGLImageFromDMABuf(opts)
}

function destroyEGLImage (imageHandle) {
  return addon.destroyEGLImage(imageHandle)
}

module.exports = { sendFd, createEGLImageFromDMABuf, destroyEGLImage }


