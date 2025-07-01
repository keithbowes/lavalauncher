scdoc = dependency(
  'scdoc',
  version: '>=1.9.2',
  native: true,
  required: get_option('man-pages'),
)
if scdoc.found()
  cf = configuration_data()
  cf.set('SYSCONFDIR', sysconfdir)
  scdoc_prog = find_program(scdoc.get_variable(pkgconfig: 'scdoc'), native: true)
  sh = find_program('sh', native: true)
  mandir = get_option('mandir')
  man_files = [
    'lavalauncher.1.scd'
  ]
  foreach filename : man_files
    topic = filename.split('.')[-3].split('/')[-1]
    section = filename.split('.')[-2]
    output = '@0@.@1@'.format(topic, section)

    gen_scd = configure_file(
      configuration: cf,
      input: filename + '.in',
      output: filename,
      install: false
    )
    custom_target(
      output,
      input: gen_scd,
      output: output,
      command: [
        sh, '-c', '@0@ < @INPUT@ > doc/@1@'.format(scdoc_prog.full_path(), output)
      ],
      install: true,
      install_dir: '@0@/man@1@'.format(mandir, section)
    )
  endforeach
endif

