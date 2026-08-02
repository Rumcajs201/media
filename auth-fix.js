// Media Center authentication compatibility fix.
// The app is hosted on GitHub Pages while Firebase Auth uses a Firebase Hosting authDomain.
// Redirect sign-in can lose the pending auth state across these origins in modern browsers.
// Use Firebase's popup flow instead; it keeps the result on the Media Center origin.
(function () {
  if (!window.firebase || !firebase.auth) return;
  try {
    const auth = firebase.auth();
    auth.signInWithRedirect = function (provider) {
      return auth.signInWithPopup(provider);
    };
  } catch (error) {
    console.error('Media Center auth compatibility fix failed:', error);
  }
})();
