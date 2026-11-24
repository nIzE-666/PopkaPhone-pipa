// Align navigator identity with the Firefox auth User-Agent on Google Sign-In pages
// so accounts.google.com does not reject embedded QtWebEngine as an insecure browser.
(() => {
    const host = String(location.hostname || "").toLowerCase();
    if (!/(^|\.)(accounts\.google\.com|accounts\.youtube\.com|gds\.google\.com)$/.test(host))
        return;
    const ua = "Mozilla/5.0 (X11; Linux x86_64; rv:136.0) Gecko/20100101 Firefox/136.0";
    const def = (obj, prop, value) => {
        try {
            Object.defineProperty(obj, prop, { get: () => value, configurable: true });
        } catch (e) {}
    };
    const proto = Object.getPrototypeOf(navigator);
    def(proto, "userAgent", ua);
    def(proto, "appVersion", "5.0 (X11)");
    def(proto, "vendor", "");
    def(proto, "userAgentData", undefined);
})();
