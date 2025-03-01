//! based on the proof of concept at ../../src/poc/direct.c

use libnotcurses_sys::*;

fn main() -> NcResult<()> {
    let mut dm = NcD::new()?;

    let dimy = dm.dim_y();
    let dimx = dm.dim_x();
    for _ in 0..dimy {
        for _ in 0..dimx {
            printf!("X");
        }
    }
    dm.flush()?;

    dm.set_fg_rgb(0xff8080)?;
    dm.styles_on(NCSTYLE_STANDOUT)?;
    printf!(" erp erp \n");
    dm.set_fg_rgb(0x80ff80)?;
    printf!(" erp erp \n");
    dm.styles_off(NCSTYLE_STANDOUT)?;
    printf!(" erp erp \n");
    dm.set_fg_rgb(0xff8080)?;
    printf!(" erp erp \n");
    dm.cursor_right(dimx / 2)?;
    dm.cursor_up(dimy / 2)?;
    printf!(" erperperp! \n");

    let (mut y, x);

    if let Ok((_y, _x)) = dm.cursor_yx() {
        y = _y;
        x = _x;
        printf!("\n\tRead cursor position: y: %d x: %d\n", y, x);

        y += 2;
        while y > 3 {
            let up = if y >= 3 { 3 } else { y };
            dm.cursor_up(up)?;
            dm.flush()?;
            y -= up;

            let newy;
            if let Ok((_y, _)) = dm.cursor_yx() {
                newy = _y;
            } else {
                break;
            }

            if newy != y {
                eprintln!("Expected {}, got {}", y, newy);
                break;
            }
            printf!("\n\tRead cursor position: y: %d x: %d\n", newy, x);
            y += 2;
        }
    } else {
        return Err(NcError::with_msg(-10, "Couldn't read cursor position."));
    }

    Ok(())
}
