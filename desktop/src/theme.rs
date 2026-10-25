//! The Gecko TV look from the console menu: its palette, Ark Pixel for
//! headings and controls, Atkinson Hyperlegible for lists and body text.
use eframe::egui::{
    self, Color32, CornerRadius, FontData, FontDefinitions, FontFamily, FontId, RichText, Stroke,
};
use std::sync::Arc;

pub const BG: Color32 = Color32::from_rgb(16, 16, 25);
pub const HEADER: Color32 = Color32::from_rgb(24, 24, 41);
pub const PANEL: Color32 = Color32::from_rgb(49, 49, 66);
pub const SELECT: Color32 = Color32::from_rgb(66, 66, 90);
pub const LINE: Color32 = Color32::from_rgb(82, 82, 107);
pub const DIM: Color32 = Color32::from_rgb(107, 107, 132);
pub const TEXT: Color32 = Color32::from_rgb(255, 255, 239);
pub const MUTED: Color32 = Color32::from_rgb(197, 205, 222);
pub const LIME: Color32 = Color32::from_rgb(132, 255, 33);
pub const OSD: Color32 = Color32::from_rgb(99, 255, 66);
pub const ORANGE: Color32 = Color32::from_rgb(255, 123, 24);
pub const RED: Color32 = Color32::from_rgb(230, 49, 33);
pub const AMBER: Color32 = Color32::from_rgb(247, 181, 74);
pub const INK: Color32 = Color32::from_rgb(8, 8, 16);

/// Ark Pixel's 12 px design, drawn at a whole multiple so it stays crisp.
pub fn pixel(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("pixel".into()))
}
/// Ark Pixel's 10 px design (2x = 20 px), for tabs and short labels.
pub fn pixel_small(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("pixel10".into()))
}
pub fn bold(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("bold".into()))
}

pub fn install(ctx: &egui::Context) {
    let mut fonts = FontDefinitions::default();
    for (name, data) in [
        (
            "ark12",
            &include_bytes!("../../vendor/fonts/ark-pixel-12px-latin.ttf")[..],
        ),
        (
            "ark10",
            &include_bytes!("../../vendor/fonts/ark-pixel-10px-latin.ttf")[..],
        ),
        (
            "atkinson",
            &include_bytes!("../../vendor/fonts/AtkinsonHyperlegible-Regular.ttf")[..],
        ),
        (
            "atkinson-bold",
            &include_bytes!("../../vendor/fonts/AtkinsonHyperlegible-Bold.ttf")[..],
        ),
    ] {
        fonts
            .font_data
            .insert(name.into(), Arc::new(FontData::from_static(data)));
    }
    // egui's own fonts stay as fallbacks for symbols the bundled ones lack.
    let fallback = fonts.families[&FontFamily::Proportional].clone();
    let family = |first: &str| [vec![first.to_owned()], fallback.clone()].concat();
    fonts
        .families
        .insert(FontFamily::Proportional, family("atkinson"));
    fonts
        .families
        .insert(FontFamily::Name("bold".into()), family("atkinson-bold"));
    fonts
        .families
        .insert(FontFamily::Name("pixel".into()), family("ark12"));
    fonts
        .families
        .insert(FontFamily::Name("pixel10".into()), family("ark10"));
    ctx.set_fonts(fonts);

    let mut visuals = egui::Visuals::dark();
    visuals.panel_fill = BG;
    visuals.window_fill = HEADER;
    visuals.window_stroke = Stroke::new(2., LINE);
    visuals.window_corner_radius = CornerRadius::ZERO;
    visuals.menu_corner_radius = CornerRadius::ZERO;
    visuals.extreme_bg_color = INK;
    visuals.faint_bg_color = HEADER;
    visuals.code_bg_color = INK;
    visuals.hyperlink_color = LIME;
    visuals.warn_fg_color = AMBER;
    visuals.error_fg_color = RED;
    visuals.override_text_color = Some(TEXT);
    visuals.selection.bg_fill = LIME;
    visuals.selection.stroke = Stroke::new(1., INK);
    for (widgets, fill, text) in [
        (&mut visuals.widgets.noninteractive, BG, MUTED),
        (&mut visuals.widgets.inactive, PANEL, TEXT),
        (&mut visuals.widgets.hovered, SELECT, TEXT),
        (&mut visuals.widgets.active, LIME, INK),
        (&mut visuals.widgets.open, SELECT, TEXT),
    ] {
        widgets.bg_fill = fill;
        widgets.weak_bg_fill = fill;
        widgets.corner_radius = CornerRadius::ZERO;
        widgets.fg_stroke = Stroke::new(1.5, text);
        widgets.bg_stroke = Stroke::NONE;
        widgets.expansion = 0.;
    }
    visuals.widgets.noninteractive.bg_stroke = Stroke::new(1., LINE);
    // Text boxes and checkboxes need an edge against the near-black page.
    visuals.widgets.inactive.bg_stroke = Stroke::new(1., LINE);
    visuals.widgets.hovered.bg_stroke = Stroke::new(1., LINE);
    ctx.set_visuals(visuals);
    ctx.style_mut(|s| {
        s.spacing.item_spacing = egui::vec2(8., 8.);
        s.spacing.button_padding = egui::vec2(12., 6.);
        s.spacing.interact_size.y = 30.;
        use egui::TextStyle::*;
        s.text_styles.insert(Body, FontId::proportional(15.));
        s.text_styles.insert(Button, FontId::proportional(15.));
        s.text_styles.insert(Small, FontId::proportional(13.));
        s.text_styles.insert(Monospace, FontId::monospace(13.));
        s.text_styles.insert(Heading, pixel(24.));
    });
}

/// A page title in the menu's style: pixel capitals over an orange and lime bar.
pub fn heading(ui: &mut egui::Ui, text: &str) {
    ui.add_space(4.);
    ui.horizontal(|ui| {
        ui.label(
            RichText::new(text.to_uppercase())
                .font(pixel(24.))
                .color(TEXT),
        );
        let (rect, _) = ui.allocate_exact_size(egui::vec2(124., 24.), egui::Sense::hover());
        let y = rect.center().y;
        ui.painter().rect_filled(
            egui::Rect::from_min_size(egui::pos2(rect.left() + 4., y - 2.), egui::vec2(40., 4.)),
            0.,
            ORANGE,
        );
        ui.painter().rect_filled(
            egui::Rect::from_min_size(egui::pos2(rect.left() + 48., y - 2.), egui::vec2(76., 4.)),
            0.,
            LIME,
        );
    });
}
/// A small pixel-font label for sidebar and section headings.
pub fn section(ui: &mut egui::Ui, text: &str) {
    ui.label(RichText::new(text).font(pixel_small(20.)).color(MUTED));
}
/// The main action on a page: a lime key, as A is on the console.
pub fn primary(text: impl Into<String>) -> egui::Button<'static> {
    egui::Button::new(RichText::new(text.into()).font(bold(15.)).color(INK)).fill(LIME)
}
/// A destructive or cancelling action: the remote's red key.
pub fn danger(text: impl Into<String>) -> egui::Button<'static> {
    egui::Button::new(RichText::new(text.into()).color(TEXT)).fill(RED)
}
/// A toggle chip; lime when chosen.
pub fn chip(selected: bool, text: impl Into<String>) -> egui::Button<'static> {
    let text = RichText::new(text.into());
    if selected {
        egui::Button::new(text.font(bold(15.)).color(INK)).fill(LIME)
    } else {
        egui::Button::new(text.color(MUTED)).fill(PANEL)
    }
}
