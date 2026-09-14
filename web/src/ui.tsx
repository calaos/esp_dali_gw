/**
 * Form and feedback primitives. Everything here composes the classes already in styles.css; the
 * only additions are the layout classes documented in web/DESIGN.md.
 */

import type { ComponentChildren } from 'preact';
import { useId, useState } from 'preact/hooks';

import { MASKED } from './config.ts';

export type Tone = 'ok' | 'warn' | 'error' | 'offline' | 'busy';

export function toneClass(tone: Tone): string {
    return `is-${tone}`;
}

/* ------------------------------------------------------------------ layout */

export function Section({
    title,
    description,
    restart,
    children,
}: {
    title: string;
    description?: string;
    /** Every field in the section needs a restart: flag it once instead of on each row. */
    restart?: boolean;
    children: ComponentChildren;
}) {
    return (
        <section class="panel section">
            <h2>
                {title}
                {restart === true && (
                    <span class="badge is-warn field__flag">
                        <span class="dot" />
                        Restart
                    </span>
                )}
            </h2>
            {description !== undefined && <p class="muted section__lede">{description}</p>}
            <div class="field-grid">{children}</div>
        </section>
    );
}

function Label({
    id,
    label,
    restart,
}: {
    id: string;
    label: string;
    restart: boolean | undefined;
}) {
    return (
        <label for={id}>
            {label}
            {restart === true && (
                <span class="badge is-warn field__flag">
                    <span class="dot" />
                    Restart
                </span>
            )}
        </label>
    );
}

interface FieldShell {
    label: string;
    hint?: string;
    /** Marks a field whose change only takes effect after a restart (SPEC §6). */
    restart?: boolean;
    disabled?: boolean;
}

function Hint({ id, hint }: { id: string; hint: string | undefined }) {
    if (hint === undefined) return null;
    return (
        <p id={id} class="muted field__hint">
            {hint}
        </p>
    );
}

/* ------------------------------------------------------------------ fields */

export function TextField({
    label,
    hint,
    restart,
    disabled,
    value,
    onInput,
    type = 'text',
    placeholder,
    mono,
    autocomplete,
    inputMode,
}: FieldShell & {
    value: string;
    onInput: (value: string) => void;
    type?: string;
    placeholder?: string;
    mono?: boolean;
    autocomplete?: string;
    inputMode?: 'text' | 'numeric' | 'url' | 'email';
}) {
    const id = useId();
    return (
        <div class="field">
            <Label id={id} label={label} restart={restart} />
            <div class="field__control">
                <input
                    id={id}
                    type={type}
                    class={mono === true ? 'mono' : undefined}
                    value={value}
                    disabled={disabled ?? false}
                    placeholder={placeholder ?? ''}
                    autocomplete={autocomplete ?? 'off'}
                    inputMode={inputMode ?? 'text'}
                    aria-describedby={hint === undefined ? undefined : `${id}-hint`}
                    onInput={(event) => {
                        onInput(event.currentTarget.value);
                    }}
                />
                <Hint id={`${id}-hint`} hint={hint} />
            </div>
        </div>
    );
}

export function NumberField({
    label,
    hint,
    restart,
    disabled,
    value,
    onInput,
    min,
    max,
    unit,
}: FieldShell & {
    value: number;
    onInput: (value: number) => void;
    min: number;
    max: number;
    unit?: string;
}) {
    const id = useId();
    return (
        <div class="field">
            <Label id={id} label={label} restart={restart} />
            <div class="field__control">
                <div class="field__unit">
                    <input
                        id={id}
                        type="number"
                        class="mono"
                        value={value}
                        min={min}
                        max={max}
                        step={1}
                        disabled={disabled ?? false}
                        inputMode="numeric"
                        aria-describedby={hint === undefined ? undefined : `${id}-hint`}
                        onInput={(event) => {
                            const parsed = Number(event.currentTarget.value);
                            if (!Number.isNaN(parsed)) onInput(parsed);
                        }}
                    />
                    {unit !== undefined && <span class="muted">{unit}</span>}
                </div>
                <Hint id={`${id}-hint`} hint={hint} />
            </div>
        </div>
    );
}

export function SelectField({
    label,
    hint,
    restart,
    disabled,
    value,
    onInput,
    options,
}: FieldShell & {
    value: string;
    onInput: (value: string) => void;
    options: { value: string; label: string }[];
}) {
    const id = useId();
    return (
        <div class="field">
            <Label id={id} label={label} restart={restart} />
            <div class="field__control">
                <select
                    id={id}
                    value={value}
                    disabled={disabled ?? false}
                    aria-describedby={hint === undefined ? undefined : `${id}-hint`}
                    onChange={(event) => {
                        onInput(event.currentTarget.value);
                    }}
                >
                    {options.map((option) => (
                        <option key={option.value} value={option.value}>
                            {option.label}
                        </option>
                    ))}
                </select>
                <Hint id={`${id}-hint`} hint={hint} />
            </div>
        </div>
    );
}

export function CheckField({
    label,
    hint,
    restart,
    disabled,
    checked,
    onChange,
}: FieldShell & { checked: boolean; onChange: (checked: boolean) => void }) {
    const id = useId();
    return (
        <div class="field field--check">
            <span class="field__spacer" aria-hidden="true" />
            <div class="field__control">
                <label class="check" for={id}>
                    <input
                        id={id}
                        type="checkbox"
                        checked={checked}
                        disabled={disabled ?? false}
                        aria-describedby={hint === undefined ? undefined : `${id}-hint`}
                        onChange={(event) => {
                            onChange(event.currentTarget.checked);
                        }}
                    />
                    <span>{label}</span>
                    {restart === true && (
                        <span class="badge is-warn field__flag">
                            <span class="dot" />
                            Restart
                        </span>
                    )}
                </label>
                <Hint id={`${id}-hint`} hint={hint} />
            </div>
        </div>
    );
}

/**
 * A password the device will not show us. `value` is the draft leaf: `MASKED` while it holds
 * whatever is stored, or whatever the user typed. Leaving the field alone keeps `MASKED` in the
 * draft, so the diff in config.ts drops it from the request body and the stored secret survives;
 * an empty box the user opened on purpose is a deliberate "remove this password".
 */
export function SecretField({
    label,
    hint,
    restart,
    value,
    onInput,
    autocomplete,
}: FieldShell & { value: string; onInput: (value: string) => void; autocomplete?: string }) {
    const id = useId();
    // Nothing stored means nothing to protect: show the box straight away rather than making the
    // user click "Change" on an empty field during first-run setup.
    const [editing, setEditing] = useState(value !== MASKED);
    const [seen, setSeen] = useState(value);
    const stored = value === MASKED;

    // The form was rebased on a document the device just sent, so the typed password is gone and
    // the field has to go back to showing what is stored.
    if (seen !== value) {
        setSeen(value);
        if (stored) setEditing(false);
    }

    if (!editing) {
        return (
            <div class="field">
                <Label id={id} label={label} restart={restart} />
                <div class="field__control">
                    <div class="field__stored">
                        <span class="muted">{stored ? 'Stored on the device' : 'Not set'}</span>
                        <button
                            id={id}
                            type="button"
                            class="btn btn--secondary"
                            onClick={() => {
                                setEditing(true);
                            }}
                        >
                            Change
                        </button>
                    </div>
                    <Hint id={`${id}-hint`} hint={hint} />
                </div>
            </div>
        );
    }

    return (
        <div class="field">
            <Label id={id} label={label} restart={restart} />
            <div class="field__control">
                <div class="field__row">
                    <input
                        id={id}
                        type="password"
                        value={stored ? '' : value}
                        autocomplete={autocomplete ?? 'new-password'}
                        placeholder={stored ? 'New password' : ''}
                        aria-describedby={`${id}-hint`}
                        onInput={(event) => {
                            onInput(event.currentTarget.value);
                        }}
                    />
                    {stored && (
                        <button
                            type="button"
                            class="btn btn--ghost"
                            onClick={() => {
                                setEditing(false);
                            }}
                        >
                            Cancel
                        </button>
                    )}
                </div>
                <p id={`${id}-hint`} class="muted field__hint">
                    {stored
                        ? 'Type a new password, or cancel to keep the stored one.'
                        : (hint ?? '')}
                </p>
            </div>
        </div>
    );
}

/* ---------------------------------------------------------------- feedback */

export function Notice({
    tone,
    title,
    children,
}: {
    tone: Tone;
    title?: string;
    children?: ComponentChildren;
}) {
    return (
        <div class={`notice ${toneClass(tone)}`} role={tone === 'error' ? 'alert' : 'status'}>
            {title !== undefined && <strong>{title}</strong>}
            {children}
        </div>
    );
}

/** Determinate progress. `value` is 0-1; pass `indeterminate` when there is nothing to divide. */
export function Meter({
    value,
    label,
    indeterminate,
}: {
    value: number;
    label: string;
    indeterminate?: boolean;
}) {
    const percent = Math.round(Math.min(Math.max(value, 0), 1) * 100);
    return (
        <div class="meter" role="group" aria-label={label}>
            <div
                class={indeterminate === true ? 'meter__track meter__track--pending' : 'meter__track'}
                role="progressbar"
                aria-valuemin={0}
                aria-valuemax={100}
                {...(indeterminate === true ? {} : { 'aria-valuenow': percent })}
            >
                <div class="meter__fill" style={indeterminate === true ? undefined : { width: `${percent}%` }} />
            </div>
            <span class="mono meter__value">{indeterminate === true ? '' : `${percent}%`}</span>
        </div>
    );
}

/**
 * Two-step confirmation, inline rather than a modal: no focus trap to get wrong, and it stays put
 * under the thumb on a phone.
 */
export function ConfirmButton({
    label,
    question,
    confirmLabel,
    danger,
    disabled,
    onConfirm,
}: {
    label: string;
    question: string;
    confirmLabel: string;
    danger?: boolean;
    disabled?: boolean;
    onConfirm: () => void;
}) {
    const [armed, setArmed] = useState(false);
    if (!armed) {
        return (
            <button
                type="button"
                class={`btn ${danger === true ? 'btn--danger' : 'btn--secondary'}`}
                disabled={disabled ?? false}
                onClick={() => {
                    setArmed(true);
                }}
            >
                {label}
            </button>
        );
    }
    return (
        <div class={`confirm ${danger === true ? 'is-error' : 'is-warn'}`} role="group">
            <p>{question}</p>
            <div class="row">
                <button
                    type="button"
                    class={`btn ${danger === true ? 'btn--danger' : 'btn--primary'}`}
                    onClick={() => {
                        setArmed(false);
                        onConfirm();
                    }}
                >
                    {confirmLabel}
                </button>
                <button
                    type="button"
                    class="btn btn--ghost"
                    onClick={() => {
                        setArmed(false);
                    }}
                >
                    Keep things as they are
                </button>
            </div>
        </div>
    );
}

/**
 * Destructive confirmation. A single click — or a single `confirm()` — is not a decision; typing
 * the word is, and it is the only gesture in the UI that cannot be made by accident.
 */
export function TypeToConfirm({
    word,
    label,
    question,
    confirmLabel,
    onConfirm,
}: {
    word: string;
    label: string;
    question: string;
    confirmLabel: string;
    onConfirm: () => void;
}) {
    const id = useId();
    const [armed, setArmed] = useState(false);
    const [typed, setTyped] = useState('');

    if (!armed) {
        return (
            <button
                type="button"
                class="btn btn--danger"
                onClick={() => {
                    setArmed(true);
                }}
            >
                {label}
            </button>
        );
    }
    return (
        <div class="confirm is-error" role="group">
            <p>{question}</p>
            <label for={id}>
                Type {word} to confirm
            </label>
            <input
                id={id}
                class="mono"
                value={typed}
                autocomplete="off"
                autocapitalize="characters"
                spellcheck={false}
                onInput={(event) => {
                    setTyped(event.currentTarget.value);
                }}
            />
            <div class="row">
                <button
                    type="button"
                    class="btn btn--danger"
                    disabled={typed.trim().toUpperCase() !== word}
                    onClick={() => {
                        setArmed(false);
                        setTyped('');
                        onConfirm();
                    }}
                >
                    {confirmLabel}
                </button>
                <button
                    type="button"
                    class="btn btn--ghost"
                    onClick={() => {
                        setArmed(false);
                        setTyped('');
                    }}
                >
                    Keep the settings
                </button>
            </div>
        </div>
    );
}
