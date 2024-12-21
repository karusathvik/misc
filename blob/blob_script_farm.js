// ==UserScript==
// @name         Full Reset with Space-W-F Sequence - Hold W, Spam Space/T/F with Delay
// @namespace    http://tampermonkey.net/
// @version      3.3
// @description  Starts by pressing Space and holding W repeatedly for 5 seconds, then spams W, T, and F concurrently. Ensures keys are unpressed before restart, with a 2-second delay on page load. Switches between URLs if leaderboard has multiple entries.
// @match        http://custom.client.blobgame.io/?ip=139.99.8.156:4801
// @match        http://custom.client.blobgame.io/?ip=139.99.8.156:4802
// @match        http://custom.client.blobgame.io/?ip=139.99.8.156:4803
// @match        http://custom.client.blobgame.io/?ip=139.99.8.156:4804
// @match        http://custom.client.blobgame.io/?ip=139.99.8.156:4805
// @match        http://custom.client.blobgame.io/?ip=139.99.8.156:4806
// @match        http://custom.client.blobgame.io/?ip=139.99.8.156:4807
// @grant        none
// ==/UserScript==

(function() {
    'use strict';

    let wHoldInterval, spaceInterval, wSpamInterval, tSpamInterval, fSpamInterval;
    let paused = false; // Flag to control the pause state

    // List of URLs to rotate through
    const urls = [
        "http://custom.client.blobgame.io/?ip=139.99.8.156:4801",
        "http://custom.client.blobgame.io/?ip=139.99.8.156:4802",
        "http://custom.client.blobgame.io/?ip=139.99.8.156:4803",
        "http://custom.client.blobgame.io/?ip=139.99.8.156:4804",
        "http://custom.client.blobgame.io/?ip=139.99.8.156:4805",
        "http://custom.client.blobgame.io/?ip=139.99.8.156:4806",
        "http://custom.client.blobgame.io/?ip=139.99.8.156:4807"
    ];

    function getNextUrl() {
        const currentIndex = urls.indexOf(window.location.href);
        const nextIndex = (currentIndex + 1) % urls.length; // Loop back to the start if at the last index
        return urls[nextIndex];
    }

    function startMainAction() {
        if (paused) return;
        clearAllIntervals();
        releaseKeys();

        function spamF() {
            fSpamInterval = setInterval(() => {
                if (!paused) {
                    const fDownEvent = new KeyboardEvent('keydown', {
                        key: 'f',
                        code: 'KeyF',
                        keyCode: 'F'.charCodeAt(0),
                        bubbles: true
                    });
                    const fUpEvent = new KeyboardEvent('keyup', {
                        key: 'f',
                        code: 'KeyF',
                        keyCode: 'F'.charCodeAt(0),
                        bubbles: true
                    });
                    document.dispatchEvent(fDownEvent);
                    document.dispatchEvent(fUpEvent);
                }
            }, 100);
        }

        function spaceThenHoldWFor5Seconds() {
            const startTime = Date.now();
            spaceInterval = setInterval(() => {
                if (!paused && (Date.now() - startTime) < 5000) {
                    const spaceDownEvent = new KeyboardEvent('keydown', {
                        key: ' ',
                        code: 'Space',
                        keyCode: 32,
                        bubbles: true
                    });
                    const spaceUpEvent = new KeyboardEvent('keyup', {
                        key: ' ',
                        code: 'Space',
                        keyCode: 32,
                        bubbles: true
                    });
                    document.dispatchEvent(spaceDownEvent);
                    document.dispatchEvent(spaceUpEvent);

                    const wDownEvent = new KeyboardEvent('keydown', {
                        key: 'w',
                        code: 'KeyW',
                        keyCode: 'W'.charCodeAt(0),
                        bubbles: true
                    });
                    document.dispatchEvent(wDownEvent);
                } else if ((Date.now() - startTime) >= 5000) {
                    clearInterval(spaceInterval);
                    spamWandT();
                }
            }, 500);
        }

        function spamWandT() {
            wSpamInterval = setInterval(() => {
                if (!paused) {
                    const wDownEvent = new KeyboardEvent('keydown', {
                        key: 'w',
                        code: 'KeyW',
                        keyCode: 'W'.charCodeAt(0),
                        bubbles: true
                    });
                    const wUpEvent = new KeyboardEvent('keyup', {
                        key: 'w',
                        code: 'KeyW',
                        keyCode: 'W'.charCodeAt(0),
                        bubbles: true
                    });
                    document.dispatchEvent(wDownEvent);
                    document.dispatchEvent(wUpEvent);
                }
            }, 100);

            tSpamInterval = setInterval(() => {
                if (!paused) {
                    const tDownEvent = new KeyboardEvent('keydown', {
                        key: 't',
                        code: 'KeyT',
                        keyCode: 'T'.charCodeAt(0),
                        bubbles: true
                    });
                    const tUpEvent = new KeyboardEvent('keyup', {
                        key: 't',
                        code: 'KeyT',
                        keyCode: 'T'.charCodeAt(0),
                        bubbles: true
                    });
                    document.dispatchEvent(tDownEvent);
                    document.dispatchEvent(tUpEvent);
                }
            }, 100);
        }

        spaceThenHoldWFor5Seconds();
        spamF();
    }

    function releaseKeys() {
        const keys = ['f', 'w', 't'];
        keys.forEach(key => {
            const keyUpEvent = new KeyboardEvent('keyup', {
                key: key,
                code: `Key${key.toUpperCase()}`,
                keyCode: key.toUpperCase().charCodeAt(0),
                bubbles: true
            });
            document.dispatchEvent(keyUpEvent);
        });
    }

    function checkExitDialog() {
        const exitDialog = document.querySelector('#exit-dialog');

        if (exitDialog && exitDialog.style.display === 'block') {
            console.log('Exit dialog detected. Refreshing page.');
            paused = true;
            clearAllIntervals();
            releaseKeys();

            setTimeout(() => {
                const leaderboard = document.querySelectorAll('#leader-board ul li');
                const singleEntry = leaderboard.length === 1 && leaderboard[0].getAttribute('uid') === "0";

                if (singleEntry) {
                    window.location.reload();
                } else {
                    window.location.href = getNextUrl(); // Switch to the next URL if multiple entries exist
                }
            }, 2500);
        }
    }

    function clearAllIntervals() {
        if (wHoldInterval) clearInterval(wHoldInterval);
        if (spaceInterval) clearInterval(spaceInterval);
        if (wSpamInterval) clearInterval(wSpamInterval);
        if (tSpamInterval) clearInterval(tSpamInterval);
        if (fSpamInterval) clearInterval(fSpamInterval);
    }

    const observer = new MutationObserver(() => {
        checkExitDialog();
    });

    observer.observe(document.body, { childList: true, subtree: true });

    window.addEventListener('load', () => {
        setTimeout(() => {
            checkExitDialog();
            clearAllIntervals();
            releaseKeys();
            startMainAction();
        }, 3500);
    });

})();
