const s_ShadowRoot = require("../Symbols").ElementShadowRoot;

function proxyStyleSet(el, styles, name, value) {
    let p = Canvas.prototype.getParent.call(el);

    switch (name) {
        case "width":
        case "height":
            if (value && value[value.length -1] == "%") {
                if (!p) break;
                el[name] = p[name] * parseInt(value) / 100;
            } else {
                el[name] = value;
            }
            break;
    
        case "left":
        case "right":
            if (value && value[value.length -1] == "%") {
                if (!p) break;
                el[name] = p.width * parseInt(value) / 100;
            } else {
                el[name] = value;
            }
            break;
    
        case "top":
        case "bottom":
            if (value && value[value.length -1] == "%") {
                if (!p) break;
                el[name] = p.height * parseInt(value) / 100;
            } else {
                el[name] = value;
            }
            break;
    }

    styles[name] = value;
}

function refreshStyles(el, styles) {
    for (k of ["width", "height", "left", "top", "bottom", "right"]) {
        if (!styles[k]) continue;
        proxyStyleSet(el, styles, k, styles[k]);
    }
}

class ElementStyles {
    constructor(el) {
        this.el = el;

        el.addEventListener("resize", () => {
            refreshStyles(el, this);
        });

        el.addEventListener("load", () => {
            var classes = el.attributes.class
            if (classes) {
                let nss;
                if (el.shadowRoot) {
                    // If element is a ShadowRoot, we need to get the styling
                    // information from the parent ShadowRoot
                    nss = Canvas.prototype.getParent.apply(el)[s_ShadowRoot].getNSS();
                } else {
                    nss = this.el[s_ShadowRoot].getNSS();
                }

                let tmp = [];
                for (let c of classes.split(" ")) {
                    tmp.push(nss[c]);
                }

                // Gives priority to variables already defined
                tmp.push(Object.assign({}, this));

                // Merge all style into |this|
                tmp.unshift(this);
                Object.assign.apply(null, tmp);
            }

            refreshStyles(el, this);
            // Needed to bypass the shadowroot
            let p = Canvas.prototype.getParent.apply(el);
            p.addEventListener("resize", () => {
                refreshStyles(el, this);
            });
        });

        return new Proxy(this, {
            set: (object, key, value, proxy) => {
                proxyStyleSet(el, object, key, value);
                return true;
            },
        });
    }

    paint(ctx) {
        let borderWidth = this.borderWidth || 1;
        
        if (this.backgroundColor) {
            ctx.fillStyle = this.backgroundColor;
            ctx.fillRect(0, 0, this.el.width, this.el.height);
        }

        if (this.borderColor) {
            ctx.strokeStyle = this.borderColor;
            ctx.strokeRect(0.5, 0.5, this.el.width-1, this.el.height-1);
        }
    }
}

module.exports = ElementStyles;