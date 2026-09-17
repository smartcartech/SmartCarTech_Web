function minus(n,t){return n-t}function plus(n,t){return n+t}

// Nut Zalo dung chung: chi can doi so dien thoai tai dong zaloPhone ben duoi.
(function () {
    "use strict";

    var zaloPhone = "0374489282";
    var zaloUrl = "https://zalo.me/" + zaloPhone;

    function initZaloContact() {
        // Cac lien ket Zalo trong menu/footer cung dung chung so dien thoai nay.
        document.querySelectorAll("a[data-zalo-contact]").forEach(function (link) {
            link.href = zaloUrl;
            link.target = "_blank";
            link.rel = "noopener noreferrer";
        });

        // Tranh tao trung nut neu script duoc nap lai.
        if (document.getElementById("zalo-contact")) {
            return;
        }

        var button = document.createElement("a");
        button.id = "zalo-contact";
        button.className = "zalo-contact";
        button.href = zaloUrl;
        button.target = "_blank";
        button.rel = "noopener noreferrer";
        button.setAttribute("lang", "vi");
        button.setAttribute("aria-label", "Nhắn tin Zalo với SmartCarTech (mở trong tab mới)");
        button.title = "Nhắn tin Zalo với SmartCarTech";
        button.innerHTML =
            '<svg class="zalo-contact__icon" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" aria-hidden="true" focusable="false">' +
                '<path fill="#fff" d="M32 9C18.7 9 8 18.2 8 29.5c0 6.6 3.6 12.5 9.3 16.3l-2 8.2 10.2-4.6c2.1.5 4.3.7 6.5.7 13.3 0 24-9.2 24-20.6S45.3 9 32 9Z"/>' +
                '<text x="32" y="35.5" text-anchor="middle" fill="#0068ff" font-family="Arial, sans-serif" font-size="17" font-weight="700">Zalo</text>' +
            '</svg>' +
            '<span class="zalo-contact__label" aria-hidden="true">Nhắn tin Zalo</span>';
        document.body.appendChild(button);
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", initZaloContact, { once: true });
    } else {
        initZaloContact();
    }
}());
