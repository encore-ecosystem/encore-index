#!/usr/bin/env python3
"""Visual checks against the local preview. Playwright is test-only, not frontend JS."""
import argparse
from pathlib import Path
from playwright.sync_api import sync_playwright


def contrast(first, second):
    def luminance(rgb):
        channels = [int(x.strip()) / 255 for x in rgb.removeprefix("rgb(").removesuffix(")").split(",")]
        linear = [c / 12.92 if c <= .04045 else ((c + .055) / 1.055) ** 2.4 for c in channels]
        return sum(c * w for c, w in zip(linear, (.2126, .7152, .0722)))
    values = sorted((luminance(first), luminance(second)))
    return (values[1] + .05) / (values[0] + .05)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8137")
    parser.add_argument("--package", default="demo_math")
    parser.add_argument("--browser", help="Optional local Chromium executable")
    parser.add_argument("--output", type=Path, default=Path("target/ui-screenshots"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(executable_path=args.browser)
        for theme in ("light", "dark"):
            for width in (375, 768, 1440):
                context = browser.new_context(viewport={"width": width, "height": 1000}, color_scheme=theme, permissions=["clipboard-read", "clipboard-write"])
                page = context.new_page()
                errors = []
                page.on("pageerror", lambda error: errors.append(str(error)))
                page.on("console", lambda message: errors.append(message.text) if message.type == "error" else None)
                requests = []
                page.on("request", lambda request: requests.append(request.url))
                for name, path in (("catalogue", "/"), ("package", "/packages/" + args.package), ("empty", "/?q=unfindable-package")):
                    response = page.goto(args.url + path)
                    assert response.status == 200
                    assert page.locator("script").count() == 1
                    assert page.locator("script").get_attribute("src") == "/assets/copy.js"
                    if name != "empty":
                        copy = page.get_by_role("button", name="Copy encore add " + args.package, exact=True)
                        copy.click()
                        page.locator("[data-copied]").wait_for()
                        assert page.evaluate("navigator.clipboard.readText()") == "encore add " + args.package
                        page.evaluate("window.scrollTo(0, 0)")
                    assert page.evaluate("document.documentElement.scrollWidth <= innerWidth"), (theme, width, path)
                    assert page.locator("h1").is_visible()
                    assert " ".join(page.locator("header .brand").inner_text().split()) == "Encore Index"
                    assert "Encore Index" in page.title()
                    assert page.locator("header [role=search]").count() == 1
                    assert page.get_by_role("searchbox", name="Find a package").is_visible()
                    assert page.locator("header").bounding_box()["height"] <= page.locator("main").bounding_box()["y"]
                    assert page.get_by_role("contentinfo").is_visible()
                    assert page.get_by_role("navigation", name="Footer navigation").is_visible()
                    if name == "package":
                        metadata = page.get_by_role("link", name="Package metadata (JSON)")
                        assert context.request.get(args.url + metadata.get_attribute("href")).status == 200
                        page.locator("summary").first.click()
                        assert page.locator(".checksum").first.is_visible()
                    if name == "empty":
                        assert page.get_by_role("searchbox").input_value() == "unfindable-package"
                        assert page.get_by_text("No packages found.").is_visible()
                    page.screenshot(path=str(args.output / f"{name}-{theme}-{width}.png"), full_page=True)
                page.goto(args.url)
                page.keyboard.press("Tab")
                assert page.locator(":focus").inner_text() == "Skip to content"
                assert page.locator(":focus").evaluate("el => getComputedStyle(el).outlineStyle") != "none"
                page.get_by_role("searchbox").fill(args.package)
                page.get_by_role("button", name="Search").click()
                assert page.get_by_role("searchbox").input_value() == args.package
                assert page.locator(".package-row").count() == 1
                colors = page.evaluate("""() => {
                    const color = s => getComputedStyle(document.querySelector(s)).color;
                    return { bg: getComputedStyle(document.body).backgroundColor,
                      text: color('body'), muted: color('.muted'),
                      button: color('button'), buttonBg: getComputedStyle(document.querySelector('button')).backgroundColor };
                }""")
                for key in ("text", "muted"):
                    assert contrast(colors[key], colors["bg"]) >= 4.5, (theme, key, colors)
                assert contrast(colors["button"], colors["buttonBg"]) >= 4.5
                assert colors["buttonBg"] == colors["text"], "primary action must remain monochrome"
                page.get_by_role("link", name="Clear search").click()
                assert page.get_by_role("searchbox").input_value() == ""
                page.get_by_role("link", name="Install →").first.click()
                assert page.locator("#install").is_visible()
                page.goto(args.url)
                assert page.locator(".brand-mark").count() == 0
                if theme == "dark":
                    assert colors["bg"] == "rgb(5, 5, 6)"
                # Stress the actual layout with long unbroken user content.
                page.locator(".package-row h3 a").first.evaluate("el => el.textContent = 'x'.repeat(64)")
                page.locator(".package-row p").first.evaluate("el => el.textContent = 'description'.repeat(90)")
                assert page.evaluate("document.documentElement.scrollWidth <= innerWidth"), (theme, width, page.evaluate("() => [...document.querySelectorAll('body *')].filter(el => el.getBoundingClientRect().right > innerWidth + 1).map(el => [el.tagName, el.className, el.getBoundingClientRect().right]).slice(0, 10)"))
                page.screenshot(path=str(args.output / f"long-content-{theme}-{width}.png"), full_page=True)
                assert not errors, errors
                assert all(url.startswith(args.url + "/") for url in requests), requests
                page.goto(args.url + "/packages/" + args.package)
                page.evaluate("() => { navigator.clipboard.writeText = () => Promise.reject(new Error('Denied')); }")
                page.get_by_role("button", name="Copy encore add " + args.package, exact=True).click()
                page.locator("#copy-status:not(.sr-only)").wait_for()
                assert "Copy manually: encore add " + args.package in page.locator("#copy-status").inner_text()
                context.close()
        context = browser.new_context(java_script_enabled=False)
        page = context.new_page()
        page.goto(args.url + "/packages/" + args.package)
        assert not page.locator(".copy-button").is_visible()
        assert "encore add " + args.package in page.locator("#install").inner_text()
        context.close()
        browser.close()
    print("Browser checks passed: both themes, 3 viewport sizes, search, focus, contrast, details and no external assets/scripts")


if __name__ == "__main__":
    main()
