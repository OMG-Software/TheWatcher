import React from 'react';
import { fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { App } from '../App';
import { clickBackdrop, dragOntoBackdrop } from '../testing/dialog';
import { FakeApi } from '../testing/fakeApi';
import { Modal } from './Modal';

type HarnessProps = Partial<React.ComponentProps<typeof Modal>> & { body?: React.ReactNode };

/** A page with a button that opens the Modal; the Modal goes away when the page stops rendering it. */
function Harness({ body, ...props }: HarnessProps) {
  const [open, setOpen] = React.useState(false);
  return (
    <>
      <button onClick={() => setOpen(true)}>open</button>
      {open ? (
        <Modal onClose={() => setOpen(false)} title="Example" {...props}>
          {body ?? (
            <>
              <button>first</button>
              <button>last</button>
            </>
          )}
        </Modal>
      ) : null}
    </>
  );
}

/** Render the page, focus the opener and click it, as a user does. Returns the opener. */
function openFrom(ui: React.ReactElement) {
  render(ui);
  const opener = screen.getByRole('button', { name: 'open' });
  opener.focus();
  fireEvent.click(opener);
  return opener;
}

const dialog = () => screen.getByRole('dialog');
const pressEscape = () => fireEvent.keyDown(document.activeElement ?? document.body, { key: 'Escape' });

afterEach(() => {
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

describe('GIVEN a Modal opened from a button', () => {
  it('WHEN it opens THEN it is a native dialog shown as a modal and named by its title', () => {
    const showModal = vi.spyOn(HTMLDialogElement.prototype, 'showModal');

    openFrom(<Harness />);

    const shown = screen.getByRole('dialog', { name: 'Example' });
    expect(shown.tagName).toBe('DIALOG');
    expect(shown).toHaveAttribute('open');
    expect(showModal.mock.contexts).toEqual([shown]);
  });

  it('WHEN it opens THEN focus moves to the first control after the header', () => {
    openFrom(<Harness />);

    expect(document.activeElement).toBe(screen.getByRole('button', { name: 'first' }));
  });

  it('WHEN a control is marked data-autofocus THEN that control takes focus instead', () => {
    openFrom(<Harness body={<><button>first</button><input aria-label="Note" data-autofocus /></>} />);

    expect(document.activeElement).toBe(screen.getByRole('textbox', { name: 'Note' }));
  });

  it('WHEN Escape is pressed THEN it closes and focus returns to the opener', () => {
    const opener = openFrom(<Harness />);

    pressEscape();

    expect(screen.queryByRole('dialog')).toBeNull();
    expect(document.activeElement).toBe(opener);
  });

  it('WHEN the close button is clicked THEN it closes and focus returns to the opener', () => {
    const opener = openFrom(<Harness closeLabel="Close example" />);

    fireEvent.click(screen.getByRole('button', { name: 'Close example' }));

    expect(screen.queryByRole('dialog')).toBeNull();
    expect(document.activeElement).toBe(opener);
  });

  it('WHEN the backdrop is clicked THEN it closes and focus returns to the opener', () => {
    const opener = openFrom(<Harness />);

    clickBackdrop(dialog());

    expect(screen.queryByRole('dialog')).toBeNull();
    expect(document.activeElement).toBe(opener);
  });

  it('WHEN the dialog box itself is clicked between its parts THEN it stays open', () => {
    openFrom(<Harness />);
    vi.spyOn(dialog(), 'getBoundingClientRect').mockReturnValue(new DOMRect(100, 100, 400, 300));

    fireEvent.pointerDown(dialog(), { clientX: 200, clientY: 200 });
    fireEvent.click(dialog(), { clientX: 200, clientY: 200 });

    expect(dialog()).toHaveAttribute('open');
  });

  it('WHEN a press starts inside and is released on the backdrop (dragging a text selection) THEN it stays open', () => {
    openFrom(<Harness />);

    dragOntoBackdrop(dialog(), screen.getByRole('button', { name: 'first' }));

    expect(dialog()).toHaveAttribute('open');
  });

  it('WHEN backdrop clicks are turned off THEN a backdrop click leaves it open and Escape still closes it', () => {
    openFrom(<Harness closeOnBackdrop={false} />);

    clickBackdrop(dialog());
    expect(dialog()).toHaveAttribute('open');

    pressEscape();
    expect(screen.queryByRole('dialog')).toBeNull();
  });

  it('WHEN it is not dismissible (a request is running) THEN Escape and the backdrop leave it open and the close button is disabled', () => {
    const onClose = vi.fn();
    openFrom(<Harness dismissible={false} onClose={onClose} />);

    pressEscape();
    clickBackdrop(dialog());

    expect(dialog()).toHaveAttribute('open');
    expect(screen.getByRole('button', { name: 'Close' })).toBeDisabled();
    expect(onClose).not.toHaveBeenCalled();
  });

  it('WHEN it is not dismissible THEN Escape never becomes a close request, which a browser cannot always cancel', () => {
    openFrom(<Harness dismissible={false} />);
    const cancel = vi.fn();
    dialog().addEventListener('cancel', cancel);

    pressEscape();

    expect(cancel).not.toHaveBeenCalled();
    expect(dialog()).toHaveAttribute('open');
  });

  it('WHEN Escape is pressed but the page keeps rendering it THEN it stays open, because only the page closes it', () => {
    const onClose = vi.fn();
    openFrom(<Harness onClose={onClose} />);

    pressEscape();

    expect(onClose).toHaveBeenCalledOnce();
    expect(dialog()).toHaveAttribute('open');
  });

  it('WHEN the browser closes it by itself while the page still renders it THEN it opens again', () => {
    openFrom(<Harness dismissible={false} />);

    (dialog() as HTMLDialogElement).close();

    expect(dialog()).toHaveAttribute('open');
  });

  it('WHEN a control inside handles Escape itself THEN the dialog stays open', () => {
    const inner = <input aria-label="Search" onKeyDown={(event) => { if (event.key === 'Escape') event.preventDefault(); }} />;
    openFrom(<Harness body={inner} />);

    fireEvent.keyDown(screen.getByRole('textbox', { name: 'Search' }), { key: 'Escape' });

    expect(dialog()).toHaveAttribute('open');
  });

  it('WHEN the header shows custom content THEN the dialog is still named by its title', () => {
    openFrom(<Harness title="Configure web-01" titleContent={<span>web-01 details</span>} />);

    const shown = screen.getByRole('dialog', { name: 'Configure web-01' });
    expect(within(shown).getByText('web-01 details')).toBeInTheDocument();
  });
});

describe('GIVEN the agent Configure dialog', () => {
  async function openConfigure() {
    new FakeApi('global_admin').install();
    render(<App />);
    fireEvent.click(within(await screen.findByRole('navigation', { name: 'Dashboard views' })).getByRole('button', { name: 'Agents' }));
    const opener = await screen.findByRole('button', { name: 'Configure' });
    opener.focus();
    fireEvent.click(opener);
    return opener;
  }

  it('WHEN it opens THEN it is a native modal dialog named after the agent', async () => {
    await openConfigure();

    const shown = screen.getByRole('dialog', { name: 'Configure web-01' });
    expect(shown.tagName).toBe('DIALOG');
    expect(shown).toHaveAttribute('open');
  });

  it('WHEN its backdrop is clicked THEN it stays open, so unsaved edits are kept', async () => {
    await openConfigure();

    clickBackdrop(dialog());

    expect(screen.getByRole('dialog', { name: 'Configure web-01' })).toHaveAttribute('open');
  });

  it('WHEN Escape is pressed THEN it closes and focus returns to the Configure button', async () => {
    const opener = await openConfigure();

    pressEscape();

    expect(screen.queryByRole('dialog')).toBeNull();
    expect(document.activeElement).toBe(opener);
  });
});
